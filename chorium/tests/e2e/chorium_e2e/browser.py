# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Using the browser app the way a person does, through the remote control:
the address bar, the ⋮ menu, long press / right click on page elements."""

import time

from . import cdp
from .remote import CTRL, VK_ESCAPE, VK_RETURN


class Browser:
    def __init__(self, target):
        self.t = target

    @property
    def r(self):
        return self.t.remote

    # --- the toolbar ---------------------------------------------------------

    def toolbar_texts(self):
        page = self.r.page_rect()
        top = page.y if page else 120
        return [t for t in self.r.texts() if t.y + t.h <= top]

    def focus_address(self):
        if not self.t.phone:
            self.r.key(ord("L"), CTRL)
            return
        # The widest text in the toolbar is the address (or its placeholder).
        texts = [t for t in self.toolbar_texts()
                 if t.text not in ("Tor", "Direct")]
        if not texts:
            raise AssertionError("no address bar on screen")
        self.r.tap(*max(texts, key=lambda t: t.w).center)

    def go(self, url, wait_part=None):
        """Types |url| into the address bar and opens it; returns its page."""
        self.focus_address()
        time.sleep(0.3)
        self.r.type(url)
        self.r.key(VK_RETURN)
        return self.page(wait_part or url.split("://", 1)[-1])

    def page(self, url_part, timeout=30):
        return cdp.page_with_url(self.t.devtools_port, url_part, timeout)

    def open_menu(self):
        self.r.tap_icon("menu")
        self.r.wait_text("Settings", exact=True)

    def settings_row(self, label):
        """Scroll the real settings page until the row is on screen."""
        for _ in range(16):
            surface = self.r.elements()
            row = self.r.find_text(label, exact=True)
            if row and 90 < row.y < surface["height"] - 45:
                return row
            x = surface["width"] // 2
            h = surface["height"]
            if self.t.phone:
                self.r.swipe(x, h - 100, x, h // 3, 300)
            else:
                self.r.wheel(x, h // 2, 300)
            time.sleep(0.15)
        raise AssertionError(f"settings row {label!r} is not visible")

    def close_menus(self):
        if self.t.phone:
            if self.r.shown("Settings", exact=True):
                self.r.back()
        else:
            self.r.key(VK_ESCAPE)
        time.sleep(0.3)

    def tor_enabled(self):
        return self.r.state()["tor_enabled"]

    def set_tor(self, on):
        if self.tor_enabled() == on:
            return
        self.open_menu()
        self.r.tap_text("Tor", exact=True)  # the menu row (drawn last)
        self.r.wait_for(lambda: self.tor_enabled() == on, 10,
                        f"Tor switched {'on' if on else 'off'}")
        self.close_menus()

    # --- pages ------------------------------------------------------------------

    def screen_point(self, page, selector):
        """Where |selector|'s centre is on the screen (CSS pixels)."""
        x, y, w, h = page.rect(selector)
        time.sleep(0.3)  # scrollIntoView
        x, y, w, h = page.rect(selector)
        box = self.r.page_rect()
        if not box:
            raise AssertionError("no page on screen")
        return box.x + x + w / 2, box.y + y + h / 2

    def tap_element(self, page, selector):
        self.r.tap(*self.screen_point(page, selector))

    def context_menu(self, page, selector):
        """Right click (desktop) or long press (phone) on |selector|."""
        x, y = self.screen_point(page, selector)
        if self.t.phone:
            self.r.press(x, y, 900)
        else:
            self.r.tap(x, y, right=True)
        time.sleep(0.6)
