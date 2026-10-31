# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""A small DevTools protocol client (stdlib only: HTTP + WebSocket)."""

import base64
import json
import os
import socket
import struct
import time
import urllib.request


def targets(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list",
                                timeout=10) as r:
        return [t for t in json.load(r) if t.get("type") == "page"]


class Page:
    """One page target. eval() runs JavaScript in the page's main world."""

    def __init__(self, ws_url):
        host_port, path = ws_url[len("ws://"):].split("/", 1)
        host, port = host_port.split(":")
        self._sock = socket.create_connection((host, int(port)), 10)
        self._sock.settimeout(60)
        key = base64.b64encode(os.urandom(16)).decode()
        self._sock.sendall(
            (f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\n"
             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
             ).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("DevTools closed the connection")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(head.decode(errors="replace"))
        self._buf = head.split(b"\r\n\r\n", 1)[1]
        self._next_id = 1

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass

    def _send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            header = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self._sock.sendall(header + mask + masked)

    def _read(self, n):
        while len(self._buf) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("DevTools closed the connection")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _recv(self):
        message = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read(8))[0]
            payload = self._read(n)
            if b0 & 0x0F == 0x8:
                raise ConnectionError("DevTools closed the page")
            message += payload
            if b0 & 0x80:
                return json.loads(message)

    def send(self, method, **params):
        id_ = self._next_id
        self._next_id += 1
        self._send(json.dumps({"id": id_, "method": method, "params": params}))
        while True:
            msg = self._recv()
            if msg.get("id") == id_:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})

    def eval(self, expression, await_promise=True):
        r = self.send("Runtime.evaluate", expression=expression,
                      returnByValue=True, awaitPromise=await_promise)
        if "exceptionDetails" in r:
            raise RuntimeError(f"page threw: {r['exceptionDetails']}")
        return r["result"].get("value")

    def rect(self, selector):
        """|selector|'s box in the page's CSS pixels (x, y, w, h)."""
        r = self.eval(
            f"(() => {{ const e = document.querySelector({json.dumps(selector)});"
            " if (!e) return null; e.scrollIntoView({block: 'center'});"
            " const b = e.getBoundingClientRect();"
            " return [b.x, b.y, b.width, b.height]; })()")
        if r is None:
            raise AssertionError(f"no element {selector!r} in the page")
        return r


def page_with_url(port, part, timeout=30):
    """Connects to the page whose URL contains |part|."""
    deadline = time.time() + timeout
    while True:
        try:
            for t in targets(port):
                if part in t.get("url", ""):
                    page = Page(t["webSocketDebuggerUrl"])
                    try:
                        state = page.eval("[location.href, document.readyState]")
                        if part in state[0] and state[1] != "loading":
                            return page
                    except RuntimeError:
                        pass  # navigation replaced the execution context
                    page.close()
        except (OSError, ConnectionError):
            pass
        if time.time() > deadline:
            raise AssertionError(f"no page with {part!r} in its URL")
        time.sleep(0.3)
