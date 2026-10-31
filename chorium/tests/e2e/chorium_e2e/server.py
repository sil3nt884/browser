# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""The pages the tests open, served from this PC on 127.0.0.1:8765 (the
phone reaches it through adb reverse). Pages connect directly to it, so the
tests switch Tor off first: loopback goes through Tor otherwise."""

import http.server
import os
import struct
import threading
import time
import urllib.parse
import uuid
import zlib

PORT = 8765
PAGES = os.path.join(os.path.dirname(os.path.dirname(__file__)), "pages")


def png(width, height, rgb):
    """A plain one-colour PNG."""
    def chunk(kind, data):
        return (struct.pack("!I", len(data)) + kind + data +
                struct.pack("!I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    row = b"\x00" + bytes(rgb) * width
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0,
                                       0)) +
            chunk(b"IDAT", zlib.compress(row * height)) + chunk(b"IEND", b""))


def file_body(name, size):
    unit = f"chorium-e2e {name}\n".encode()
    return (unit * (size // len(unit) + 1))[:size]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, kind, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        parts = url.path.strip("/").split("/", 1)
        if parts[0] == "pages" and len(parts) == 2:
            path = os.path.join(PAGES, os.path.basename(parts[1]))
            if os.path.isfile(path):
                with open(path, "rb") as f:
                    kind = ("text/html; charset=utf-8"
                            if path.endswith(".html") else
                            "text/javascript" if path.endswith(".js") else
                            "application/octet-stream")
                    body = f.read()
                    if path.endswith("index.html"):
                        body = body.replace(b"chorium-e2e-file.txt", self.server.file_name.encode())
                        body = body.replace(b"chorium-e2e-slow.bin", self.server.slow_name.encode())
                        body = body.replace(b"cat.png", self.server.image_name.encode())
                    return self.send(200, body, kind)
        if url.path == "/" + self.server.image_name:
            return self.send(200, png(120, 90, (230, 120, 40)), "image/png")
        if parts[0] == "download" and len(parts) == 2:
            name = os.path.basename(parts[1])
            size = int(q.get("size", ["2048"])[0])
            return self.send(200, file_body(name, size),
                             "application/octet-stream",
                             [("Content-Disposition",
                               f'attachment; filename="{name}"')])
        if parts[0] == "slow" and len(parts) == 2:
            # 1 MB over ~30 s: long enough to cancel.
            name = os.path.basename(parts[1])
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(1 << 20))
            self.send_header("Content-Disposition",
                             f'attachment; filename="{name}"')
            self.end_headers()
            try:
                for _ in range(64):
                    self.wfile.write(b"x" * (1 << 14))
                    self.wfile.flush()
                    time.sleep(0.5)
            except OSError:
                pass  # cancelled
            return
        self.send(404, b"not found", "text/plain")


class Server:
    def __init__(self):
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", PORT),
                                                     Handler)
        token = uuid.uuid4().hex
        self.file_name = self.httpd.file_name = f"chorium-e2e-{token}.txt"
        self.slow_name = self.httpd.slow_name = f"chorium-e2e-{token}-slow.bin"
        self.image_name = self.httpd.image_name = f"chorium-e2e-{token}.png"
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{PORT}"

    def url(self, path):
        return self.base + path

    def stop(self):
        self.httpd.shutdown()
