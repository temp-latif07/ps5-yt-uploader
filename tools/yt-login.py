#!/usr/bin/env python3
"""One-time isolated-browser YouTube Studio login helper for PS5 YouTube Uploader.

Opens a temporary browser profile directly to https://studio.youtube.com/, waits for
you to sign in (or switch to a Brand Account with --choose-channel), captures cookies
and active ytcfg metadata via Chrome DevTools Protocol, POSTs them to the console,
and deletes the temporary browser profile so daily browsing never rotates those cookies.

Usage:
    python3 tools/yt-login.py http://<ps5-ip>:8888
    python3 tools/yt-login.py http://<ps5-ip>:8888 --choose-channel
    python3 tools/yt-login.py --self-test
"""

import argparse
import base64
import hashlib
import json
import os
import platform
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ALLOWED_SUFFIXES = (".youtube.com", "youtube.com", ".google.com", "google.com", ".youtu.be", "youtu.be")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def allowed_domain(domain: str) -> bool:
    d = (domain or "").strip().lower()
    if not d:
        return False
    for s in ALLOWED_SUFFIXES:
        if d == s or (s.startswith(".") and d.endswith(s)) or (not s.startswith(".") and d.endswith("." + s)):
            return True
    return False


def cookies_to_netscape(cookies: list) -> str:
    lines = ["# Netscape HTTP Cookie File"]
    for c in cookies:
        domain = c.get("domain", "")
        if not allowed_domain(domain):
            continue
        name = c.get("name", "")
        value = c.get("value", "")
        if not name:
            continue
        prefix = "#HttpOnly_" if c.get("httpOnly") else ""
        flag = "TRUE" if domain.startswith(".") else "FALSE"
        path = c.get("path") or "/"
        secure = "TRUE" if c.get("secure") else "FALSE"
        exp = c.get("expires", 0)
        try:
            exp_int = int(exp) if exp and float(exp) > 0 else 2147483647
        except (TypeError, ValueError):
            exp_int = 2147483647
        lines.append(f"{prefix}{domain}\t{flag}\t{path}\t{secure}\t{exp_int}\t{name}\t{value}")
    return "\n".join(lines) + "\n"


def has_required_cookies(cookies: list) -> bool:
    names = {c.get("name") for c in cookies if allowed_domain(c.get("domain", ""))}
    return ("SAPISID" in names or "__Secure-3PAPISID" in names) and ("SID" in names or "__Secure-3PSID" in names)


class SimpleWS:
    """Minimal RFC 6455 WebSocket client using only the Python standard library."""

    def __init__(self, ws_url: str, timeout: float = 10.0):
        u = urllib.parse.urlparse(ws_url)
        host = u.hostname or "127.0.0.1"
        port = u.port or 80
        path = u.path + (f"?{u.query}" if u.query else "")
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("WebSocket handshake closed")
            buf += chunk
        head, rest = buf.split(b"\r\n\r\n", 1)
        expected = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        if b"101" not in head.split(b"\r\n", 1)[0] or expected.encode("ascii") not in head:
            raise ConnectionError("WebSocket upgrade failed")
        self.buf = bytearray(rest)
        self.msg_id = 0

    def _read_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("WebSocket closed")
            self.buf.extend(chunk)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        mask = os.urandom(4)
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _recv_message(self) -> str:
        fragments = []
        while True:
            b1, b2 = self._read_exact(2)
            fin = (b1 & 0x80) != 0
            opcode = b1 & 0x0F
            masked = (b2 & 0x80) != 0
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if masked else b""
            data = self._read_exact(length)
            if masked:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if opcode == 0x8:
                raise ConnectionError("WebSocket peer closed")
            if opcode == 0x9:
                self._send_frame(0xA, data)
                continue
            if opcode == 0xA:
                continue
            fragments.append(data)
            if fin:
                return b"".join(fragments).decode("utf-8", errors="replace")

    def call(self, method: str, params: dict = None) -> dict:
        self.msg_id += 1
        mid = self.msg_id
        self._send_frame(0x1, json.dumps({"id": mid, "method": method, "params": params or {}}).encode("utf-8"))
        while True:
            msg = json.loads(self._recv_message())
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(msg["error"].get("message", "CDP error"))
                return msg.get("result", {})

    def close(self) -> None:
        try:
            self._send_frame(0x8, b"")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def find_browser(explicit: str = "") -> str:
    if explicit:
        return explicit if os.path.exists(explicit) else ""
    sys_name = platform.system()
    candidates = []
    if sys_name == "Darwin":
        candidates = [
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
            "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser",
            "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        ]
    elif sys_name == "Windows":
        for root in (os.environ.get("ProgramFiles", ""), os.environ.get("ProgramFiles(x86)", ""), os.environ.get("LocalAppData", "")):
            if root:
                candidates.extend([
                    os.path.join(root, "Google", "Chrome", "Application", "chrome.exe"),
                    os.path.join(root, "Microsoft", "Edge", "Application", "msedge.exe"),
                    os.path.join(root, "BraveSoftware", "Brave-Browser", "Application", "brave.exe"),
                ])
    else:
        candidates = ["google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "brave-browser", "microsoft-edge"]

    for c in candidates:
        if os.path.isabs(c) and os.path.isfile(c):
            return c
        found = shutil.which(c)
        if found:
            return found
    return ""


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def fetch_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=3) as r:
        return json.loads(r.read().decode("utf-8"))


def read_studio_ytcfg(ws_url: str) -> dict:
    ws = SimpleWS(ws_url)
    try:
        expr = (
            "(function(){var d=(window.ytcfg&&window.ytcfg.data_)||{};"
            "return JSON.stringify({channel_id:d.CHANNEL_ID||'',"
            "delegated_session_id:d.DELEGATED_SESSION_ID||'',"
            "session_index:String(d.SESSION_INDEX!==undefined?d.SESSION_INDEX:'0')});})()"
        )
        res = ws.call("Runtime.evaluate", {"expression": expr, "returnByValue": True})
        val = res.get("result", {}).get("value", "{}")
        return json.loads(val) if isinstance(val, str) else {}
    finally:
        ws.close()


def read_cdp_cookies(ws_url: str) -> list:
    ws = SimpleWS(ws_url)
    try:
        res = ws.call("Network.getAllCookies")
        return res.get("cookies", [])
    finally:
        ws.close()


def push_to_console(ps5_url: str, netscape: str, ytcfg: dict) -> dict:
    base = ps5_url.rstrip("/")
    payload = {
        "cookies": netscape,
        "channel_id": ytcfg.get("channel_id", ""),
        "delegated_session_id": ytcfg.get("delegated_session_id", ""),
        "session_index": ytcfg.get("session_index", "0"),
    }
    data = urllib.parse.urlencode(payload).encode("utf-8")
    req = urllib.request.Request(
        f"{base}/api/login",
        data=data,
        headers={"X-PS5YT": "1", "Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        try:
            msg = json.loads(body).get("error", body)
        except Exception:
            msg = body
        raise RuntimeError(f"PS5 refused login (HTTP {e.code}): {msg}")


def capture_via_cdp(browser_bin: str, choose_channel: bool, timeout: int) -> tuple:
    port = free_port()
    profile = tempfile.mkdtemp(prefix="ps5yt-login-")
    cmd = [
        browser_bin,
        f"--user-data-dir={profile}",
        f"--remote-debugging-port={port}",
        "--remote-allow-origins=*",
        "--no-first-run",
        "--no-default-browser-check",
        "https://studio.youtube.com/",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + timeout
        cdp_base = f"http://127.0.0.1:{port}"
        while time.time() < deadline:
            try:
                fetch_json(f"{cdp_base}/json/version")
                break
            except Exception:
                if proc.poll() is not None:
                    raise RuntimeError("Browser exited before remote debugging started")
                time.sleep(0.25)
        else:
            raise TimeoutError("Timed out waiting for browser to start")

        if choose_channel:
            print("Sign in to Google in the opened window, switch to your Brand Account if desired,")
            input("then press Enter here once YouTube Studio is loaded on the target channel...")

        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("Browser was closed before login finished")
            pages = fetch_json(f"{cdp_base}/json/list")
            studio_page = next(
                (p for p in pages if p.get("type") == "page" and "studio.youtube.com" in p.get("url", "")
                 and "accounts.google.com" not in p.get("url", "")),
                None,
            )
            if studio_page and studio_page.get("webSocketDebuggerUrl"):
                ws_url = studio_page["webSocketDebuggerUrl"]
                cookies = read_cdp_cookies(ws_url)
                ytcfg = read_studio_ytcfg(ws_url)
                if has_required_cookies(cookies) and ytcfg.get("channel_id"):
                    return cookies, ytcfg
            if choose_channel:
                raise RuntimeError("YouTube Studio session not ready yet — finish signing in and retry")
            time.sleep(0.8)

        raise TimeoutError("Timed out waiting for YouTube Studio login")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)


def capture_via_playwright(choose_channel: bool, timeout: int) -> tuple:
    from playwright.sync_api import sync_playwright

    profile = tempfile.mkdtemp(prefix="ps5yt-pw-")
    try:
        with sync_playwright() as p:
            ctx = p.chromium.launch_persistent_context(profile, headless=False)
            page = ctx.pages[0] if ctx.pages else ctx.new_page()
            page.goto("https://studio.youtube.com/")
            if choose_channel:
                print("Sign in to Google in the opened window, switch to your Brand Account if desired,")
                input("then press Enter here once YouTube Studio is loaded...")
            deadline = time.time() + timeout
            while time.time() < deadline:
                if "studio.youtube.com" in page.url and "accounts.google.com" not in page.url:
                    cookies = ctx.cookies(["https://studio.youtube.com", "https://youtube.com", "https://google.com"])
                    ytcfg = page.evaluate(
                        "()=>{var d=(window.ytcfg&&window.ytcfg.data_)||{};"
                        "return {channel_id:d.CHANNEL_ID||'',delegated_session_id:d.DELEGATED_SESSION_ID||'',"
                        "session_index:String(d.SESSION_INDEX!==undefined?d.SESSION_INDEX:'0')};}"
                    )
                    if has_required_cookies(cookies) and ytcfg.get("channel_id"):
                        ctx.close()
                        return cookies, ytcfg
                time.sleep(0.8)
            ctx.close()
            raise TimeoutError("Timed out waiting for YouTube Studio login")
    finally:
        shutil.rmtree(profile, ignore_errors=True)


def self_test() -> int:
    sample = [
        {"domain": ".youtube.com", "path": "/", "secure": True, "httpOnly": False, "expires": 2000000000, "name": "SAPISID", "value": "sap_val"},
        {"domain": ".google.com", "path": "/", "secure": True, "httpOnly": True, "expires": 2000000000, "name": "SID", "value": "sid_val"},
        {"domain": ".example.com", "path": "/", "secure": False, "httpOnly": False, "expires": 0, "name": "OTHER", "value": "ignore"},
    ]
    assert has_required_cookies(sample) is True
    assert has_required_cookies([sample[2]]) is False
    txt = cookies_to_netscape(sample)
    assert ".youtube.com\tTRUE\t/\tTRUE\t2000000000\tSAPISID\tsap_val" in txt
    assert "#HttpOnly_.google.com\tTRUE\t/\tTRUE\t2000000000\tSID\tsid_val" in txt
    assert "example.com" not in txt
    print("ok   tools/yt-login.py self-test passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="One-time isolated browser login helper for PS5 YouTube Uploader.")
    ap.add_argument("ps5_url", nargs="?", help="Console web UI URL, e.g. http://192.168.1.50:8888")
    ap.add_argument("--choose-channel", action="store_true", help="Pause so you can switch to a Brand Account before capturing")
    ap.add_argument("--browser", default="", help="Path to Chrome/Chromium/Edge/Brave binary")
    ap.add_argument("--timeout", type=int, default=300, help="Login timeout in seconds (default: 300)")
    ap.add_argument("--self-test", action="store_true", help="Run internal cookie serialization test and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.ps5_url:
        ap.error("ps5_url is required (e.g. python3 tools/yt-login.py http://192.168.1.50:8888)")

    browser = find_browser(args.browser)
    print("Opening an isolated browser window to https://studio.youtube.com/ ...")
    if browser:
        cookies, ytcfg = capture_via_cdp(browser, args.choose_channel, args.timeout)
    else:
        try:
            cookies, ytcfg = capture_via_playwright(args.choose_channel, args.timeout)
        except ImportError:
            print("Error: Chrome/Chromium/Edge not found and Playwright is not installed.", file=sys.stderr)
            print("Install Chrome/Chromium or run: pip install playwright && playwright install chromium", file=sys.stderr)
            return 1

    netscape = cookies_to_netscape(cookies)
    print(f"Captured cookies for channel {ytcfg.get('channel_id') or '(default)'}; sending to {args.ps5_url} ...")
    res = push_to_console(args.ps5_url, netscape, ytcfg)
    ch = res.get("channel") or {}
    print(f"Signed in on PS5 as: {ch.get('name', 'YouTube Channel')} ({ch.get('id', '')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
