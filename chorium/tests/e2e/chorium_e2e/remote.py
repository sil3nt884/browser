# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Client for the runtime's remote control (runtime/common/remote.h).

What the app drew comes back as elements (texts, icons, pages) in CSS
pixels; input goes in through the platform's real input path.
"""

import base64
import json
import socket
import time


class Element:
    def __init__(self, d):
        self.text = d.get("text")
        self.icon = d.get("icon")
        self.id = d.get("id")
        self.x, self.y, self.w, self.h = d["x"], d["y"], d["w"], d["h"]

    @property
    def center(self):
        return self.x + self.w // 2, self.y + self.h // 2

    def __repr__(self):
        what = self.text if self.text is not None else self.icon or self.id
        return f"<{what!r} at {self.x},{self.y} {self.w}x{self.h}>"


class Remote:
    def __init__(self, port, connect_timeout=60):
        deadline = time.time() + connect_timeout
        while True:
            try:
                self._sock = socket.create_connection(("127.0.0.1", port), 5)
                self._sock.settimeout(60)
                self._file = self._sock.makefile("rwb")
                self.state()  # adb forward accepts before the app listens
                return
            except (OSError, ValueError, ConnectionError):
                if time.time() > deadline:
                    raise
                time.sleep(0.5)

    def close(self):
        try:
            self._file.close()
            self._sock.close()
        except OSError:
            pass

    def call(self, cmd, **args):
        args["cmd"] = cmd
        self._file.write(json.dumps(args).encode() + b"\n")
        self._file.flush()
        line = self._file.readline()
        if not line:
            raise ConnectionError("the app closed the remote control")
        reply = json.loads(line)
        if not reply.get("ok"):
            raise RuntimeError(f"{cmd}: {reply.get('error')}")
        return reply

    # --- seeing ------------------------------------------------------------

    def elements(self):
        return self.call("elements")

    def state(self):
        return self.call("state")

    def texts(self):
        return [Element(t) for t in self.elements()["texts"]]

    def find_text(self, text, exact=False):
        """The topmost (last drawn) text matching |text|, or None."""
        for t in reversed(self.texts()):
            if (t.text == text) if exact else (text in t.text):
                return t
        return None

    def shown(self, text, exact=False):
        return self.find_text(text, exact) is not None

    def find_icon(self, icon):
        for i in reversed(self.elements()["icons"]):
            if i["icon"] == icon:
                return Element(i)
        return None

    def page_rect(self):
        """The page the app shows: Element (id, x, y, w, h), or None."""
        pages = self.elements()["pages"]
        return Element(pages[-1]) if pages else None

    def wait_for(self, check, timeout=20, what="condition"):
        deadline = time.time() + timeout
        while True:
            value = check()
            if value:
                return value
            if time.time() > deadline:
                raise AssertionError(f"timed out waiting for {what}")
            time.sleep(0.2)

    def wait_text(self, text, exact=False, timeout=20):
        return self.wait_for(lambda: self.find_text(text, exact), timeout,
                             f"text {text!r} on screen")

    def wait_gone(self, text, exact=False, timeout=20):
        self.wait_for(lambda: not self.shown(text, exact), timeout,
                      f"text {text!r} to go away")

    def screenshot(self):
        r = self.call("screenshot")
        return r["format"], base64.b64decode(r["data"])

    # --- acting ------------------------------------------------------------

    def tap(self, x, y, right=False):
        if right:
            self.call("tap", x=int(x), y=int(y), button="right")
        else:
            self.call("tap", x=int(x), y=int(y))

    def press(self, x, y, ms=900):
        self.call("press", x=int(x), y=int(y), ms=ms)

    def swipe(self, x, y, x2, y2, ms=300):
        self.call("swipe", x=int(x), y=int(y), x2=int(x2), y2=int(y2), ms=ms)

    def wheel(self, x, y, dy):
        self.call("wheel", x=int(x), y=int(y), dy=int(dy))

    def key(self, code, mods=0):
        self.call("key", code=code, mods=mods)

    def type(self, text):
        self.call("text", text=text)

    def back(self):
        self.call("back")

    def tap_text(self, text, exact=False, timeout=10):
        t = self.wait_text(text, exact, timeout)
        self.tap(*t.center)

    def tap_icon(self, icon, timeout=10):
        i = self.wait_for(lambda: self.find_icon(icon), timeout,
                          f"icon {icon!r}")
        self.tap(*i.center)

    def intercept_open(self, on=True):
        self.call("intercept_open", on=on)

    def picker_answer(self, paths):
        self.call("picker_answer", paths="\n".join(paths))


VK_BACK, VK_RETURN, VK_ESCAPE = 0x08, 0x0D, 0x1B
CTRL, SHIFT = 2, 1
