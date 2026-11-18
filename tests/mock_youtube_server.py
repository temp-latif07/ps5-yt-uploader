#!/usr/bin/env python3
"""
Mock YouTube Scotty & InnerTube Server for PS5 YouTube Uploader Tests.
Simulates:
1. Scotty Resumable Session Init (/upload/studio)
2. Scotty Chunk Upload & Disconnect Resume Query (/upload/resumable/<session_id>)
3. InnerTube Video Finalization (/youtubei/v1/upload/createvideo)
"""

import sys
import os
import json
import socket
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8999

sessions = {}

class MockYouTubeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        # Quiet logging during test runs
        if os.environ.get("VERBOSE_MOCK"):
            sys.stderr.write("[MockYouTube] " + (format % args) + "\n")

    def send_cors_and_headers(self, status, headers=None, body_bytes=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body_bytes)))
        self.send_header("Connection", "close")
        if headers:
            for k, v in headers.items():
                self.send_header(k, v)
        self.end_headers()
        if body_bytes:
            self.wfile.write(body_bytes)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path in ("/health", "/status"):
            body = json.dumps({"status": "ok"}).encode("utf-8")
            self.send_cors_and_headers(200, {"Content-Type": "application/json"}, body)
        else:
            self.send_cors_and_headers(404, {}, b'{"error": "not found"}')

    def do_POST(self):
        parsed = urlparse(self.path)
        content_len = int(self.headers.get("Content-Length", 0))
        
        # 1. Health / Reset control
        if parsed.path == "/control/reset":
            sessions.clear()
            self.send_cors_and_headers(200, {"Content-Type": "application/json"}, b'{"status": "reset"}')
            return

        # 2. Scotty Resumable Session Init: /upload/studio
        if parsed.path == "/upload/studio":
            auth = self.headers.get("Authorization", "")
            cookie = self.headers.get("Cookie", "")
            if not auth.startswith("SAPISIDHASH") or "EXPIRED_COOKIE" in cookie or "INVALID" in auth:
                err_body = json.dumps({
                    "error": {
                        "code": 401,
                        "message": "UNAUTHENTICATED: session token or cookies expired"
                    }
                }).encode("utf-8")
                self.send_cors_and_headers(401, {"Content-Type": "application/json"}, err_body)
                return

            body = self.rfile.read(content_len) if content_len > 0 else b"{}"
            try:
                data = json.loads(body.decode("utf-8")) if body else {}
            except Exception:
                data = {}

            frontend_id = data.get("frontendUploadId", "")
            session_id = f"sess_{len(sessions) + 1}_{frontend_id}"
            simulate_drop = ("simulate_drop" in frontend_id or "drop" in frontend_id)
            drop_full = ("drop_full" in frontend_id)
            query_401 = ("query_401" in frontend_id)

            sessions[session_id] = {
                "frontend_id": frontend_id,
                "bytes_received": 0,
                "data": bytearray(),
                "simulate_drop": simulate_drop,
                "drop_full": drop_full,
                "query_401": query_401,
                "drop_occurred": False,
                "finalized": False
            }

            host = self.headers.get("Host", f"127.0.0.1:{PORT}")
            upload_url = f"http://{host}/upload/resumable/{session_id}"

            resp_headers = {
                "x-goog-upload-url": upload_url,
                "x-goog-upload-status": "active",
                "x-goog-upload-chunk-granularity": "1048576",
                "Content-Type": "application/json"
            }
            self.send_cors_and_headers(200, resp_headers, b'{}')
            return

        # 3. Scotty Resumable Streaming: /upload/resumable/<session_id>
        if parsed.path.startswith("/upload/resumable/"):
            session_id = parsed.path.split("/")[-1]
            session = sessions.get(session_id)
            if not session:
                self.send_cors_and_headers(404, {}, b'{"error": "session not found"}')
                return

            auth = self.headers.get("Authorization", "")
            if not auth.startswith("SAPISIDHASH") or "INVALID" in auth:
                err_body = json.dumps({
                    "error": {
                        "code": 401,
                        "message": "UNAUTHENTICATED: session token or cookies expired"
                    }
                }).encode("utf-8")
                self.send_cors_and_headers(401, {"Content-Type": "application/json"}, err_body)
                return

            cmd = self.headers.get("x-goog-upload-command", "")

            # Query offset command
            if "query" in cmd:
                if session.get("query_401"):
                    err_body = json.dumps({
                        "error": {
                            "code": 401,
                            "message": "UNAUTHENTICATED: session token or cookies expired"
                        }
                    }).encode("utf-8")
                    self.send_cors_and_headers(401, {"Content-Type": "application/json"}, err_body)
                    return

                resp_headers = {
                    "x-goog-upload-status": "final" if session["finalized"] else "active",
                    "x-goog-upload-size-received": str(session["bytes_received"]),
                    "Content-Type": "application/json"
                }
                self.send_cors_and_headers(200, resp_headers, b'{}')
                return

            # Upload chunk command
            if "upload" in cmd:
                offset = int(self.headers.get("x-goog-upload-offset", 0))

                # Check for simulated mid-stream network drop
                if session["simulate_drop"] and not session["drop_occurred"]:
                    if session.get("drop_full"):
                        # Read entire chunk, then drop before responding
                        chunk = self.rfile.read(content_len) if content_len > 0 else b""
                        session["data"].extend(chunk)
                    else:
                        # Read partial chunk (up to 32768 bytes or half), update bytes_received, then sever socket
                        drop_bytes = min(32768, content_len // 2) if content_len > 1 else content_len
                        partial = self.rfile.read(drop_bytes)
                        session["data"].extend(partial)

                    session["bytes_received"] = len(session["data"])
                    session["drop_occurred"] = True

                    # Abruptly close connection without HTTP response
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.connection.close()
                    except Exception:
                        pass
                    return

                # Normal or resumed chunk upload
                chunk_data = self.rfile.read(content_len) if content_len > 0 else b""
                # If resuming at offset, adjust session data
                if offset == len(session["data"]):
                    session["data"].extend(chunk_data)
                elif offset < len(session["data"]):
                    # Overwrite or extend
                    session["data"] = session["data"][:offset] + chunk_data
                else:
                    session["data"].extend(chunk_data)
                session["bytes_received"] = len(session["data"])

                if "finalize" in cmd:
                    session["finalized"] = True
                    resp_body = json.dumps({
                        "sessionStatus": {"state": "FINALIZED"},
                        "scottyResourceId": "mock_scotty_resource_12345"
                    }).encode("utf-8")
                    resp_headers = {
                        "x-goog-upload-status": "final",
                        "Content-Type": "application/json"
                    }
                    self.send_cors_and_headers(200, resp_headers, resp_body)
                else:
                    resp_headers = {
                        "x-goog-upload-status": "active",
                        "x-goog-upload-size-received": str(session["bytes_received"]),
                        "Content-Type": "application/json"
                    }
                    self.send_cors_and_headers(200, resp_headers, b'{}')
                return

        # 4. InnerTube createvideo: /youtubei/v1/upload/createvideo
        if parsed.path == "/youtubei/v1/upload/createvideo":
            body = self.rfile.read(content_len) if content_len > 0 else b"{}"
            try:
                data = json.loads(body.decode("utf-8")) if body else {}
            except Exception:
                data = {}

            # Validate session token
            token = data.get("context", {}).get("request", {}).get("sessionInfo", {}).get("token", "")
            if not token or token in ("EXPIRED_TOKEN", "INVALID_TOKEN"):
                err_body = json.dumps({
                    "error": {
                        "code": 401,
                        "message": "UNAUTHENTICATED: session token or cookies expired"
                    }
                }).encode("utf-8")
                self.send_cors_and_headers(401, {"Content-Type": "application/json"}, err_body)
                return

            scotty_id = data.get("resourceId", {}).get("scottyResourceId", {}).get("id", "")
            if not scotty_id:
                err_body = json.dumps({
                    "error": {
                        "code": 400,
                        "message": "Missing scottyResourceId"
                    }
                }).encode("utf-8")
                self.send_cors_and_headers(400, {"Content-Type": "application/json"}, err_body)
                return

            resp_body = json.dumps({
                "videoId": "mock_vid_98765"
            }).encode("utf-8")
            self.send_cors_and_headers(200, {"Content-Type": "application/json"}, resp_body)
            return

        # Fallback
        self.send_cors_and_headers(404, {}, b'{"error": "endpoint not found"}')


class QuietHTTPServer(HTTPServer):
    def handle_error(self, request, client_address):
        # Ignore broken pipe / connection reset during intentional disconnect simulations
        pass

def run():
    server = QuietHTTPServer(("127.0.0.1", PORT), MockYouTubeHandler)
    # Output ready message for tests
    sys.stdout.write(f"MOCK_YOUTUBE_SERVER_READY:{PORT}\n")
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    run()
