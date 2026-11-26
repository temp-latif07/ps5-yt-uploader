#!/usr/bin/env python3
"""Mock YouTube Studio, Scotty resumable upload, and InnerTube server for host tests.

    mock_youtube.py [PORT]
"""
import hashlib
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LOCK = threading.Lock()
CTRL = {
    "drop_once_after": 0,
    "rate": 0,
    "expire_next_query": 0,
    "force_401": 0,
    "fail_metadata": 0,
}
SESSIONS = {}
NEXT_SID = 1
NEXT_VID = 1
STATS = {
    "scotty_inits": 0,
    "scotty_queries": 0,
    "createvideo": [],
    "metadata_update": [],
}


def parse_cookies(hdr: str) -> dict:
    out = {}
    for part in (hdr or "").split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if "-v" in sys.argv:
            super().log_message(fmt, *args)

    def send_json(self, code: int, obj: dict, extra_headers=None):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers:
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}

        if u.path == "/control":
            with LOCK:
                for k in ("drop_once_after", "rate", "expire_next_query", "force_401", "fail_metadata"):
                    if k in q:
                        CTRL[k] = int(q[k])
            return self.send_json(200, {"ok": True, "ctrl": CTRL})

        if u.path == "/control/stats":
            with LOCK:
                return self.send_json(200, STATS)

        if u.path.startswith("/accounts/ServiceLogin"):
            html = b"<html><body>Sign in to Google</body></html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        if u.path.startswith("/studio"):
            cookies = parse_cookies(self.headers.get("Cookie", ""))
            sap = cookies.get("SAPISID", "")
            if not sap.startswith("valid-"):
                host = self.headers.get("Host", "127.0.0.1")
                self.send_response(302)
                self.send_header("Location", f"http://{host}/accounts/ServiceLogin")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            cid = "UC_DEFAULT"
            if "/channel/" in u.path:
                cid = u.path.split("/channel/", 1)[1].strip("/") or "UC_DEFAULT"
            page_id = self.headers.get("X-Goog-PageId", "")
            sidx = q.get("authuser", self.headers.get("X-Goog-AuthUser", "0"))
            title = "Brand Gaming" if cid == "UC_BRAND" else "PS5 Test Channel"
            ytcfg = {
                "INNERTUBE_API_KEY": "mock_api_key",
                "INNERTUBE_CLIENT_VERSION": "1.20261126.00.00",
                "CHANNEL_ID": cid,
                "DELEGATED_SESSION_ID": page_id,
                "SESSION_INDEX": sidx,
                "CHANNEL_TITLE": title,
            }
            html = f"<!doctype html><html><script>ytcfg.set({json.dumps(ytcfg)});</script></html>".encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Set-Cookie", "YT_ROTATED=rotated-cookie-val; Path=/")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
            return

        self.send_json(404, {"error": "not found"})

    def do_POST(self):
        global NEXT_SID, NEXT_VID
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}

        with LOCK:
            if CTRL["force_401"]:
                return self.send_json(401, {"error": "UNAUTHENTICATED"})

        if u.path == "/upload/studio":
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("SAPISIDHASH ") or self.headers.get("Origin") != "https://studio.youtube.com":
                return self.send_json(401, {"error": "bad auth or origin"})
            if self.headers.get("x-goog-upload-command") != "start":
                return self.send_json(400, {"error": "expected start"})
            clen = int(self.headers.get("Content-Length", 0))
            _ = self.rfile.read(clen) if clen else b""
            total = int(self.headers.get("x-goog-upload-header-content-length", "0"))
            with LOCK:
                sid = f"S{NEXT_SID}"
                NEXT_SID += 1
                SESSIONS[sid] = {"buf": bytearray(), "offset": 0, "total": total, "final": False}
                STATS["scotty_inits"] += 1
            host = self.headers.get("Host", "127.0.0.1")
            up_url = f"http://{host}/upload/session/{sid}"
            return self.send_json(200, {"ok": True}, [("x-goog-upload-url", up_url)])

        if u.path.startswith("/upload/session/"):
            sid = u.path.split("/upload/session/", 1)[1]
            with LOCK:
                sess = SESSIONS.get(sid)
            if not sess:
                return self.send_json(404, {"error": "session expired"})
            cmd = (self.headers.get("x-goog-upload-command") or "").lower()
            if cmd == "query":
                with LOCK:
                    STATS["scotty_queries"] += 1
                    if CTRL["expire_next_query"] > 0:
                        CTRL["expire_next_query"] -= 1
                        SESSIONS.pop(sid, None)
                        return self.send_json(410, {"error": "expired"})
                    off = sess["offset"]
                    fin = sess["final"]
                hdrs = [("x-goog-upload-status", "final" if fin else "active"), ("x-goog-upload-size-received", str(off))]
                body = {"scottyResourceId": f"SCOTTY_{sid}"} if fin else {"status": "active"}
                return self.send_json(200, body, hdrs)

            if "upload" in cmd:
                off = int(self.headers.get("x-goog-upload-offset", "0"))
                clen = int(self.headers.get("Content-Length", "0"))
                with LOCK:
                    drop_at = CTRL["drop_once_after"]
                    rate = CTRL["rate"]
                    if drop_at > 0:
                        CTRL["drop_once_after"] = 0
                got = bytearray()
                t0 = time.time()
                rem = clen
                while rem > 0:
                    want = min(65536, rem)
                    if drop_at > 0 and len(got) + want > drop_at:
                        want = drop_at - len(got)
                    chunk = self.rfile.read(want) if want > 0 else b""
                    if not chunk:
                        break
                    got.extend(chunk)
                    rem -= len(chunk)
                    if drop_at > 0 and len(got) >= drop_at:
                        with LOCK:
                            if len(sess["buf"]) < off + len(got):
                                sess["buf"].extend(b"\x00" * (off + len(got) - len(sess["buf"])))
                            sess["buf"][off:off + len(got)] = got
                            sess["offset"] = max(sess["offset"], off + len(got))
                        try:
                            self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        self.connection.close()
                        self.close_connection = True
                        return
                    if rate > 0:
                        ahead = len(got) / rate - (time.time() - t0)
                        if ahead > 0:
                            time.sleep(ahead)

                with LOCK:
                    if len(sess["buf"]) < off + len(got):
                        sess["buf"].extend(b"\x00" * (off + len(got) - len(sess["buf"])))
                    sess["buf"][off:off + len(got)] = got
                    sess["offset"] = max(sess["offset"], off + len(got))
                    fin = "finalize" in cmd
                    if fin:
                        sess["final"] = True
                    cur_off = sess["offset"]
                hdrs = [("x-goog-upload-status", "final" if fin else "active"), ("x-goog-upload-size-received", str(cur_off))]
                body = {"scottyResourceId": f"SCOTTY_{sid}"} if fin else {"received": cur_off}
                return self.send_json(200, body, hdrs)

            return self.send_json(400, {"error": "unknown upload command"})

        if u.path == "/youtubei/v1/upload/createvideo":
            if q.get("key") != "mock_api_key":
                return self.send_json(403, {"error": "bad api key"})
            clen = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(clen).decode("utf-8"))
            ctx = data.get("context", {})
            if ctx.get("client", {}).get("clientName") != "WEB_CREATOR":
                return self.send_json(400, {"error": "expected WEB_CREATOR"})
            meta = data.get("initialMetadata", {})
            if meta.get("draftState", {}).get("isDraft") is not False:
                return self.send_json(400, {"error": "expected draftState.isDraft == false"})
            scotty_id = data.get("resourceId", {}).get("scottyResourceId", {}).get("id", "")
            sid = scotty_id.replace("SCOTTY_", "", 1)
            with LOCK:
                sess = SESSIONS.get(sid)
                digest = hashlib.sha256(bytes(sess["buf"])).hexdigest() if sess else ""
                vid = f"VID_{NEXT_VID}"
                NEXT_VID += 1
                STATS["createvideo"].append({
                    "video_id": vid,
                    "channel_id": data.get("channelId", ""),
                    "on_behalf_of": ctx.get("user", {}).get("onBehalfOfUser", ""),
                    "x_goog_pageid": self.headers.get("X-Goog-PageId", ""),
                    "title": meta.get("title", {}).get("newTitle", ""),
                    "description": meta.get("description", {}).get("newDescription", ""),
                    "privacy": meta.get("privacy", {}).get("newPrivacy", ""),
                    "scotty_id": scotty_id,
                    "size": len(sess["buf"]) if sess else 0,
                    "sha256": digest,
                })
            return self.send_json(200, {"videoId": vid})

        if u.path == "/youtubei/v1/video_manager/metadata_update":
            if q.get("key") != "mock_api_key":
                return self.send_json(403, {"error": "bad api key"})
            with LOCK:
                if CTRL["fail_metadata"] > 0:
                    CTRL["fail_metadata"] -= 1
                    return self.send_json(500, {"error": "injected metadata error"})
            clen = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(clen).decode("utf-8"))
            mfk = data.get("madeForKids", {})
            if mfk.get("operation") != "MDE_MADE_FOR_KIDS_UPDATE_OPERATION_SET":
                return self.send_json(400, {"error": "bad madeForKids operation"})
            if mfk.get("newMfk") == "MDE_MADE_FOR_KIDS_TYPE_MFK" and "addToPlaylist" in data:
                return self.send_json(400, {"error": "playlists disallowed for MFK"})
            with LOCK:
                STATS["metadata_update"].append({
                    "video_id": data.get("encryptedVideoId", ""),
                    "new_mfk": mfk.get("newMfk", ""),
                    "playlists": data.get("addToPlaylist", {}).get("addToPlaylistIds", []),
                    "x_goog_pageid": self.headers.get("X-Goog-PageId", ""),
                    "on_behalf_of": data.get("context", {}).get("user", {}).get("onBehalfOfUser", ""),
                })
            return self.send_json(200, {"ok": True})

        self.send_json(404, {"error": "unknown endpoint"})


def main():
    port = int(next((a for a in sys.argv[1:] if a.isdigit()), "18999"))
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print(f"mock YouTube Studio on http://127.0.0.1:{port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
