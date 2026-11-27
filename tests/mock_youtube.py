#!/usr/bin/env python3
"""Mock Google OAuth 2.0 Device Flow & YouTube Data API v3 server for host tests.

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
    "force_401_once": 0,
    "force_invalid_grant": 0,
    "force_quota_403": 0,
    "fail_playlist": 0,
    "approve_device": 0,
    "slow_down_once": 0,
    "rotate_refresh": 0,
}
DEVICES = {}
SESSIONS = {}
CURRENT_TOKEN = "tok_1"
NEXT_TOK = 2
NEXT_SID = 1
NEXT_VID = 1
NEXT_PLI = 1
STATS = {
    "device_starts": 0,
    "refreshes": 0,
    "playlist_lists": 0,
    "inits": 0,
    "queries": 0,
    "videos": [],
    "playlists": [],
}


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

    def send_empty(self, code: int, extra_headers=None):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        if extra_headers:
            for k, v in extra_headers:
                self.send_header(k, v)
        self.end_headers()

    def check_bearer(self) -> bool:
        auth = self.headers.get("Authorization", "")
        with LOCK:
            if CTRL["force_401_once"] > 0:
                CTRL["force_401_once"] -= 1
                return False
            want = f"Bearer {CURRENT_TOKEN}"
        return auth == want

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}

        if u.path == "/control":
            with LOCK:
                for k in CTRL:
                    if k in q:
                        CTRL[k] = int(q[k])
                if CTRL["approve_device"]:
                    for d in DEVICES.values():
                        d["approved"] = True
                    CTRL["approve_device"] = 0
            return self.send_json(200, {"ok": True, "ctrl": CTRL})

        if u.path == "/control/stats":
            with LOCK:
                return self.send_json(200, STATS)

        if u.path == "/youtube/v3/channels":
            if not self.check_bearer():
                return self.send_json(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
            return self.send_json(200, {
                "kind": "youtube#channelListResponse",
                "items": [{"id": "UC_TEST_123", "snippet": {"title": "PS5 Test Channel"}}],
            })

        if u.path == "/youtube/v3/playlists":
            if not self.check_bearer():
                return self.send_json(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
            with LOCK:
                STATS["playlist_lists"] += 1
            return self.send_json(200, {
                "kind": "youtube#playlistListResponse",
                "items": [
                    {
                        "id": "PL123",
                        "snippet": {"title": "PS5 Highlights"},
                        "contentDetails": {"itemCount": 14},
                    },
                    {
                        "id": "PL999",
                        "snippet": {"title": "Boss Fights"},
                        "contentDetails": {"itemCount": 3},
                    },
                ],
            })

        self.send_json(404, {"error": "not found"})

    def do_POST(self):
        global NEXT_SID, NEXT_PLI, CURRENT_TOKEN, NEXT_TOK
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        clen = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(clen) if clen else b""

        if u.path == "/oauth/device/code":
            form = {k: v[0] for k, v in parse_qs(raw.decode("utf-8")).items()}
            if not form.get("client_id") or form.get("scope") != "https://www.googleapis.com/auth/youtube":
                return self.send_json(400, {"error": "invalid_request", "error_description": "bad client_id or scope"})
            dc = f"DEV_CODE_{int(time.time() * 1000)}_{len(DEVICES) + 1}"
            with LOCK:
                DEVICES[dc] = {"client_id": form["client_id"], "approved": False}
                STATS["device_starts"] += 1
            return self.send_json(200, {
                "device_code": dc,
                "user_code": "ABCD-EFGH",
                "verification_url": "https://www.google.com/device",
                "expires_in": 1800,
                "interval": 1,
            })

        if u.path == "/oauth/token":
            form = {k: v[0] for k, v in parse_qs(raw.decode("utf-8")).items()}
            gt = form.get("grant_type", "")
            if gt == "urn:ietf:params:oauth:grant-type:device_code":
                dc = form.get("device_code", "")
                with LOCK:
                    dev = DEVICES.get(dc)
                    if not dev or form.get("client_id") != dev["client_id"] or not form.get("client_secret"):
                        return self.send_json(400, {"error": "invalid_client"})
                    if CTRL["slow_down_once"] > 0:
                        CTRL["slow_down_once"] -= 1
                        return self.send_json(400, {"error": "slow_down"})
                    if not dev["approved"]:
                        return self.send_json(400, {"error": "authorization_pending"})
                    DEVICES.pop(dc, None)
                    CURRENT_TOKEN = f"tok_{NEXT_TOK}"
                    NEXT_TOK += 1
                    tok = CURRENT_TOKEN
                return self.send_json(200, {
                    "access_token": tok,
                    "refresh_token": "ref_1",
                    "expires_in": 3600,
                    "token_type": "Bearer",
                })

            if gt == "refresh_token":
                if not form.get("client_id") or not form.get("client_secret") or not form.get("refresh_token"):
                    return self.send_json(400, {"error": "invalid_request"})
                with LOCK:
                    if CTRL["force_invalid_grant"] > 0:
                        CTRL["force_invalid_grant"] -= 1
                        return self.send_json(400, {"error": "invalid_grant", "error_description": "Token has been expired or revoked."})
                    CURRENT_TOKEN = f"tok_{NEXT_TOK}"
                    NEXT_TOK += 1
                    tok = CURRENT_TOKEN
                    STATS["refreshes"] += 1
                    rot = CTRL["rotate_refresh"]
                    if rot > 0:
                        CTRL["rotate_refresh"] -= 1
                resp = {"access_token": tok, "expires_in": 3600, "token_type": "Bearer"}
                if rot > 0:
                    resp["refresh_token"] = "ref_rotated"
                return self.send_json(200, resp)

            return self.send_json(400, {"error": "unsupported_grant_type"})

        if u.path == "/upload/youtube/v3/videos":
            if not self.check_bearer():
                return self.send_json(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
            with LOCK:
                if CTRL["force_quota_403"] > 0:
                    CTRL["force_quota_403"] -= 1
                    return self.send_json(403, {"error": {"errors": [{"reason": "quotaExceeded"}]}})
            if q.get("uploadType") != "resumable":
                return self.send_json(400, {"error": "expected uploadType=resumable"})
            data = json.loads(raw.decode("utf-8"))
            total = int(self.headers.get("X-Upload-Content-Length", "0"))
            mime = self.headers.get("X-Upload-Content-Type", "")
            with LOCK:
                sid = f"S{NEXT_SID}"
                NEXT_SID += 1
                SESSIONS[sid] = {
                    "buf": bytearray(),
                    "offset": 0,
                    "total": total,
                    "mime": mime,
                    "meta": data,
                    "final": False,
                    "video_id": "",
                }
                STATS["inits"] += 1
            host = self.headers.get("Host", "127.0.0.1")
            return self.send_empty(200, [("Location", f"http://{host}/upload/session/{sid}")])

        if u.path == "/youtube/v3/playlistItems":
            if not self.check_bearer():
                return self.send_json(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
            with LOCK:
                if CTRL["fail_playlist"] > 0:
                    CTRL["fail_playlist"] -= 1
                    return self.send_json(500, {"error": "injected playlist error"})
            data = json.loads(raw.decode("utf-8"))
            snip = data.get("snippet", {})
            with LOCK:
                pid = f"PLI_{NEXT_PLI}"
                NEXT_PLI += 1
                STATS["playlists"].append({
                    "playlistId": snip.get("playlistId", ""),
                    "videoId": snip.get("resourceId", {}).get("videoId", ""),
                })
            return self.send_json(200, {"id": pid})

        self.send_json(404, {"error": "unknown endpoint"})

    def do_PUT(self):
        global NEXT_VID
        u = urlparse(self.path)
        if not u.path.startswith("/upload/session/"):
            return self.send_json(404, {"error": "not found"})

        clen = int(self.headers.get("Content-Length", "0"))
        cr = self.headers.get("Content-Range", "")

        if not self.check_bearer():
            # Drain request body before responding 401 cleanly
            rem = clen
            while rem > 0:
                chunk = self.rfile.read(min(65536, rem))
                if not chunk:
                    break
                rem -= len(chunk)
            return self.send_json(401, {"error": {"code": 401, "message": "Invalid Credentials"}})

        sid = u.path.split("/upload/session/", 1)[1]
        with LOCK:
            sess = SESSIONS.get(sid)
        if not sess:
            return self.send_json(404, {"error": "session not found"})

        # Offset query: PUT with Content-Length: 0 and Content-Range: bytes */<total>
        if cr.startswith("bytes */"):
            with LOCK:
                STATS["queries"] += 1
                if CTRL["expire_next_query"] > 0:
                    CTRL["expire_next_query"] -= 1
                    SESSIONS.pop(sid, None)
                    return self.send_json(410, {"error": "upload session expired"})
                fin = sess["final"]
                vid = sess["video_id"]
                off = sess["offset"]
            if fin and vid:
                return self.send_json(200, {"id": vid})
            hdrs = [("Range", f"bytes=0-{off - 1}")] if off > 0 else []
            return self.send_empty(308, hdrs)

        # Chunk PUT: Content-Range: bytes <start>-<end>/<total>
        if not cr.startswith("bytes ") or "/" not in cr or "-" not in cr:
            return self.send_json(400, {"error": f"bad Content-Range: {cr}"})
        rng_part = cr[6:].split("/", 1)[0]
        start_s, end_s = rng_part.split("-", 1)
        start = int(start_s)
        end = int(end_s)

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
                    if len(sess["buf"]) < start + len(got):
                        sess["buf"].extend(b"\x00" * (start + len(got) - len(sess["buf"])))
                    sess["buf"][start:start + len(got)] = got
                    sess["offset"] = max(sess["offset"], start + len(got))
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
            if len(sess["buf"]) < start + len(got):
                sess["buf"].extend(b"\x00" * (start + len(got) - len(sess["buf"])))
            sess["buf"][start:start + len(got)] = got
            sess["offset"] = max(sess["offset"], start + len(got))
            cur_off = sess["offset"]
            total = sess["total"]
            if end + 1 >= total and cur_off >= total:
                sess["final"] = True
                vid = f"VID_{NEXT_VID}"
                NEXT_VID += 1
                sess["video_id"] = vid
                digest = hashlib.sha256(bytes(sess["buf"])).hexdigest()
                meta = sess["meta"]
                STATS["videos"].append({
                    "id": vid,
                    "mime": sess["mime"],
                    "size": len(sess["buf"]),
                    "sha256": digest,
                    "title": meta.get("snippet", {}).get("title", ""),
                    "description": meta.get("snippet", {}).get("description", ""),
                    "categoryId": meta.get("snippet", {}).get("categoryId", ""),
                    "privacyStatus": meta.get("status", {}).get("privacyStatus", ""),
                    "selfDeclaredMadeForKids": meta.get("status", {}).get("selfDeclaredMadeForKids"),
                    "hasPaidProductPlacement": meta.get("paidProductPlacementDetails", {}).get("hasPaidProductPlacement"),
                })
                return self.send_json(200, {"id": vid})

        return self.send_empty(308, [("Range", f"bytes=0-{cur_off - 1}")])


def main():
    port = int(next((a for a in sys.argv[1:] if a.isdigit()), "18999"))
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print(f"mock Google OAuth + YouTube Data API v3 on http://127.0.0.1:{port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
