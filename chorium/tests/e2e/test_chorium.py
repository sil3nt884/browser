# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""End-to-end tests of the real app (Chorium.exe or Chorium.apk) through
the remote control and DevTools. Run with run.py, which picks the platform.

Features of this round (context menu, downloads, uploads, frame rate,
sandbox) and regressions from earlier ones (address bar, the search box
crash, Tor and its switch, theme, session restore, full screen, back, tabs,
"offline" pages)."""

import os
from contextlib import closing
import random
import sqlite3
import struct
import time
import unittest

from chorium_e2e import cdp
from chorium_e2e.browser import Browser
from chorium_e2e.remote import CTRL, VK_ESCAPE, VK_RETURN

# Set by run.py.
TARGET = None
SERVER = None
NETWORK = False


def uid():
    return f"{random.randrange(1 << 30):08x}"


class ChoriumTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = TARGET
        cls.b = Browser(TARGET)

    def setUp(self):
        # Every scenario starts with its own clean state. Restart checks keep
        # this profile only within the scenario being tested.
        self.t.stop()
        self.t.start(fresh=True)
        self.r = self.t.remote
        self.b.close_menus()

    def home(self, direct=True):
        """The test page, opened through the address bar."""
        if direct:
            self.b.set_tor(False)
        return self.b.go(SERVER.url("/pages/index.html"), "/pages/index.html")

    def desktop_only(self):
        if self.t.phone:
            self.skipTest("desktop only")


# --- this round's features -------------------------------------------------------


class ContextMenu(ChoriumTest):
    def test_text_field_cut_copy_paste_select_all(self):
        self.desktop_only()  # Android fields keep the system's own menu
        page = self.home()
        page.eval("document.getElementById('a').value = 'hello chorium'")
        self.b.tap_element(page, "#a")
        self.b.context_menu(page, "#a")
        for label in ("Select all", "Paste"):
            self.assertTrue(self.r.shown(label, exact=True), label)
        self.r.tap_text("Select all", exact=True)
        self.b.context_menu(page, "#a")
        self.assertTrue(self.r.shown("Copy", exact=True))
        self.assertTrue(self.r.shown("Cut", exact=True))
        self.r.tap_text("Copy", exact=True)
        time.sleep(0.5)
        self.assertEqual(self.t.clipboard_text(), "hello chorium")
        self.b.tap_element(page, "#b")
        self.b.context_menu(page, "#b")
        self.r.tap_text("Paste", exact=True)
        self.r.wait_for(lambda: page.eval(
            "document.getElementById('b').value") == "hello chorium", 5,
            "the pasted text")
        # Cut empties the first field.
        self.b.context_menu(page, "#a")
        self.r.tap_text("Select all", exact=True)
        self.b.context_menu(page, "#a")
        self.r.tap_text("Cut", exact=True)
        self.r.wait_for(lambda: page.eval(
            "document.getElementById('a').value") == "", 5, "the cut")

    def test_link_menu(self):
        page = self.home()
        self.b.context_menu(page, "#link")
        for label in ("Open in new tab", "Copy link address",
                      "Save link as file"):
            self.assertTrue(self.r.shown(label), label)
        if not self.t.phone:
            self.r.tap_text("Copy link address")
            time.sleep(0.5)
            self.assertTrue(self.t.clipboard_text().endswith(
                "/pages/second.html"))
            self.b.context_menu(page, "#link")
        self.r.tap_text("Open in new tab")
        self.b.page("/pages/second.html")  # a new page opened

    def test_image_menu_copy_and_save(self):
        page = self.home()
        self.b.context_menu(page, "#img")
        for label in ("Copy image address", "Save image"):
            self.assertTrue(self.r.shown(label), label)
        self.assertTrue(self.r.shown("Copy image", exact=True))
        if not self.t.phone:
            self.r.tap_text("Copy image", exact=True)
            self.r.wait_for(self.t.clipboard_has_image, 10,
                            "an image on the clipboard")
            self.b.context_menu(page, "#img")
        name = SERVER.image_name
        self.r.tap_text("Save image")
        data = self.r.wait_for(lambda: self.t.read_download(name), 30,
                               f"{name} in Downloads")
        self.assertTrue(data.startswith(b"\x89PNG"))
        self.t.delete_download(name)


class Downloads(ChoriumTest):
    def tearDown(self):
        stem, ext = os.path.splitext(SERVER.file_name)
        for name in (SERVER.file_name, f"{stem} (1){ext}", SERVER.slow_name):
            self.t.delete_download(name)

    def test_download_lands_in_downloads_with_notice(self):
        name = SERVER.file_name
        self.t.delete_download(name)
        page = self.home()
        self.b.tap_element(page, "#dl")
        data = self.r.wait_for(lambda: self.t.read_download(name), 30,
                               f"{name} in Downloads")
        self.assertEqual(len(data), 2048)
        self.assertTrue(data.startswith(b"chorium-e2e " + name.encode()))
        self.r.wait_text(name)
        self.r.wait_text("done")
        # The same file again gets a free name, not an overwrite.
        self.b.tap_element(page, "#dl")
        stem, ext = os.path.splitext(name)
        self.r.wait_for(lambda: self.t.read_download(
            f"{stem} (1){ext}"), 30, "the second copy")

    def test_notice_and_panel_open_files(self):
        name = SERVER.file_name
        self.t.delete_download(name)
        self.r.intercept_open(True)
        try:
            page = self.home()
            self.b.tap_element(page, "#dl")
            self.r.wait_text("done")
            self.r.tap_icon("folder")  # the notice's Show in folder
            opened = self.r.state()["opened"]
            self.assertTrue(opened and opened[-1]["in_folder"])
            self.assertTrue(self.t.download_reference_matches(opened[-1]["path"], name))
            # The Downloads page, from the menu.
            self.b.open_menu()
            self.r.tap_text("Downloads", exact=True)
            self.r.wait_text("Downloads", exact=True)
            self.r.tap_text(name)
            opened = self.r.state()["opened"]
            self.assertFalse(opened[-1]["in_folder"])
            self.assertTrue(self.t.download_reference_matches(opened[-1]["path"], name))
            self.r.back() if self.t.phone else self.r.key(VK_ESCAPE)
        finally:
            self.r.intercept_open(False)

    def test_cancel(self):
        name = SERVER.slow_name
        page = self.home()
        self.b.tap_element(page, "#dlslow")
        self.r.wait_text(name)
        self.b.open_menu()
        self.r.tap_text("Downloads", exact=True)
        self.r.wait_text("Downloads", exact=True)
        self.r.tap_icon("close")  # the running download's cancel button
        self.r.wait_text("Cancelled")
        self.assertIsNone(self.t.read_download(name))


class Uploads(ChoriumTest):
    def test_file_input_opens_system_picker(self):
        page = self.home()
        if self.t.phone:
            # A file the app may read: one it downloaded itself.
            name = SERVER.file_name
            self.t.delete_download(name)
            self.b.tap_element(page, "#dl")
            self.r.wait_for(lambda: self.t.read_download(name), 30, "download")
            path = self.t.download_path(name)
            expected = self.t.read_download(name)
        else:
            name = f"upload-{uid()}.txt"
            expected = b"uploaded by the e2e test\n"
            path = self.t.upload_file(name, expected)
        self.b.tap_element(page, "#file")
        if self.t.phone:
            self.r.wait_for(lambda: self.r.state()["picker"], 10,
                            "the picker to open")
            self.r.wait_for(lambda: "documentsui" in self.t.top_activity(),
                            10, "the system file picker on screen")
            self.r.picker_answer([path])
        else:
            self.t.answer_file_dialog([path])
        picked = self.r.wait_for(lambda: page.eval("window.picked"), 15,
                                 "the page to get the file")
        self.assertEqual(picked[0]["name"], name)
        self.assertEqual(picked[0]["text"].encode(), expected)
        if self.t.phone:
            self.t.delete_download(name)

    def test_cancel_leaves_input_empty(self):
        page = self.home()
        self.b.tap_element(page, "#file")
        if self.t.phone:
            self.r.wait_for(lambda: self.r.state()["picker"], 10, "picker")
            self.r.picker_answer([])
        else:
            self.t.answer_file_dialog([])
        time.sleep(1)
        self.assertEqual(page.eval("document.getElementById('file')"
                                   ".files.length"), 0)
        self.assertIsNone(self.r.state()["picker"])


class FrameRate(ChoriumTest):
    def test_pages_draw_at_the_screen_refresh_rate(self):
        hz = self.t.display_hz()
        self.b.set_tor(False)
        page = self.b.go(SERVER.url("/pages/anim.html"), "/pages/anim.html")
        time.sleep(1)
        fps = page.eval("measure(3000)")
        print(f"\n    display {hz:.0f} Hz, page {fps:.0f} fps", end=" ")
        # Close to the display's rate, and clearly above 60 when it is.
        self.assertGreater(fps, hz * 0.8)
        if hz > 75:
            self.assertGreater(fps, 70)
        if not self.t.phone:
            # The window shows them too: frames presented per second.
            self.assertGreater(self.r.state()["fps"], min(hz, 120) * 0.6)


class Sandbox(ChoriumTest):
    def test_page_processes_are_sandboxed(self):
        self.home()
        if self.t.phone:
            users = self.t.renderer_users()
            self.assertTrue(users, "no page processes found")
            for user in users:
                self.assertRegex(user, r"^u\d+_i\d+$",
                                 "page process not an isolated user")
            return
        renderers = [p for p, kind in self.t.helper_processes()
                     if kind == "renderer"]
        self.assertTrue(renderers, "no page processes found")
        for pid in renderers:
            level = self.t.integrity_level(pid)
            self.assertIsNotNone(level, f"can't read renderer {pid}'s token")
            self.assertLessEqual(level, 0x1000,
                                 f"renderer {pid} runs at integrity {level:#x}")
            self.assertTrue(self.t.in_job(pid), f"renderer {pid} not in a job")
            self.assertTrue(self.t.win32k_disabled(pid),
                            f"renderer {pid} can still call win32k")


# --- regressions -------------------------------------------------------------------


class Regressions(ChoriumTest):
    def test_address_bar_edits_long_unicode_text(self):
        self.b.set_tor(False)
        prefix = SERVER.url("/pages/index.html") + "?q=" + "a" * 180
        self.b.focus_address()
        self.r.type(prefix + "😀é")
        self.r.wait_text(prefix + "😀é", exact=True)
        self.r.key(0x25)  # before é
        self.r.key(0x08)  # remove the entire emoji, without touching its neighbours
        self.r.type("中")
        self.r.wait_text(prefix + "中é", exact=True)
        self.r.key(VK_RETURN)
        page = self.b.page("/pages/index.html")
        self.assertEqual(page.eval("new URL(location.href).searchParams.get('q')"),
                         "a" * 180 + "中é")

    def test_address_bar_edits_middle_and_selection(self):
        self.b.set_tor(False)
        url = SERVER.url("/pages/inZZdex.html")
        self.b.focus_address()
        self.r.type(url)
        original = self.r.wait_text(url, exact=True)
        # Clicking/tapping an already focused address places a caret at that
        # visible character. A short insertion must preserve both sides.
        self.r.tap(*original.center)
        self.r.type("~")
        edited = self.r.wait_for(lambda: next(
            (t.text for t in self.r.texts() if "~" in t.text), None), 5,
            "the insertion in the address")
        self.assertEqual(edited.replace("~", ""), url)
        self.assertGreater(edited.index("~"), 0)
        self.assertLess(edited.index("~"), len(edited) - 1)
        self.r.key(0x08)
        self.r.wait_text(url, exact=True)
        original = self.r.find_text(url, exact=True)
        y = original.center[1]
        self.r.swipe(original.x + original.w // 3, y,
                     original.x + original.w * 2 // 3, y)
        self.r.type("~")
        edited = self.r.wait_for(lambda: next(
            (t.text for t in self.r.texts() if "~" in t.text), None), 5,
            "replacement of the dragged selection")
        self.assertGreater(edited.index("~"), 0)
        self.assertLess(edited.index("~"), len(edited) - 1)
        self.assertLess(len(edited), len(url))
        self.r.key(ord("A"), CTRL)
        self.r.type(url)
        # Select just the two wrong characters and remove them. Navigation
        # proves the edited value, rather than only the drawing, is correct.
        self.r.key(0x23)  # End
        for _ in "dex.html":
            self.r.key(0x25)  # Left
        self.r.key(0x25, 1)  # Shift+Left
        self.r.key(0x25, 1)
        self.r.key(0x2E)  # Delete selection
        self.r.wait_text(SERVER.url("/pages/index.html"), exact=True)
        self.r.key(VK_RETURN)
        self.b.page("/pages/index.html")

    def test_page_text_is_visible(self):
        self.desktop_only()
        page = self.home()
        page.eval("""(() => {
          const c = document.createElement('canvas');
          c.id = 'font-test'; c.width = 240; c.height = 60;
          c.style = 'position:fixed;left:20px;top:20px;background:white;z-index:999';
          document.body.append(c);
          const ctx = c.getContext('2d');
          ctx.fillStyle = '#ff0000'; ctx.font = '28px Arial';
          ctx.fillText('Visible text', 8, 40);
        })()""")
        x, y, w, h = page.rect('#font-test')
        box = self.r.page_rect()
        scale = self.r.elements()['scale']

        def red_pixels_on_screen():
            fmt, data = self.r.screenshot()
            self.assertEqual(fmt, 'bmp')
            offset = struct.unpack_from('<I', data, 10)[0]
            width, height = struct.unpack_from('<ii', data, 18)
            self.assertEqual(struct.unpack_from('<H', data, 28)[0], 32)
            count = 0
            for py in range(int((box.y + y) * scale),
                            int((box.y + y + h) * scale)):
                row = py if height < 0 else height - py - 1
                for px in range(int((box.x + x) * scale),
                                int((box.x + x + w) * scale)):
                    b, g, r = struct.unpack_from(
                        'BBB', data, offset + (row * width + px) * 4)
                    count += r > 150 and g < 120 and b < 120
            return count > 80

        self.r.wait_for(red_pixels_on_screen, 10, 'text pixels in the actual window')

    def test_site_data_clear_on_close(self):
        page = self.home()
        page.eval("document.cookie = 'close_test=1; Path=/; Max-Age=86400';"
                  "localStorage.setItem('close_test', '1')")
        self.b.open_menu()
        self.r.tap_text("Settings", exact=True)
        row = self.b.settings_row("Clear when Chorium closes")
        self.r.tap(*row.center)
        page.close()
        self.t.restart()
        self.r = self.t.remote
        page = self.home()
        self.assertNotIn("close_test=", page.eval("document.cookie"))
        self.assertIsNone(page.eval("localStorage.getItem('close_test')"))

    def test_site_data_survives_restart_and_can_be_cleared(self):
        page = self.home()
        value = uid()
        page.eval(f"document.cookie = 'persist={value}; Path=/; Max-Age=86400';"
                  f"localStorage.setItem('persist', '{value}')")
        page.eval("""(async () => {
          const db = await new Promise((resolve, reject) => {
            const request = indexedDB.open('chorium-e2e', 1);
            request.onupgradeneeded = () => request.result.createObjectStore('values');
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error);
          });
          await new Promise((resolve, reject) => {
            const tx = db.transaction('values', 'readwrite');
            tx.objectStore('values').put(localStorage.getItem('persist'), 'key');
            tx.oncomplete = resolve; tx.onerror = () => reject(tx.error);
          });
          db.close();
          const cache = await caches.open('chorium-e2e');
          await cache.put('/persisted', new Response(localStorage.getItem('persist')));
          await navigator.serviceWorker.register('/pages/worker.js');
          await navigator.serviceWorker.ready;
        })()""")
        page.close()
        self.t.stop()
        if not self.t.phone:
            path = os.path.join(self.t.profile, "profile", "Network", "Cookies")
            with closing(sqlite3.connect(path)) as db:
                plain, encrypted = db.execute(
                    "SELECT value, encrypted_value FROM cookies WHERE name='persist'"
                ).fetchone()
            self.assertEqual(plain, "", "cookie value was saved in plaintext")
            self.assertTrue(encrypted.startswith(b"v10"), "no DPAPI-backed cookie")
            self.assertNotIn(value.encode(), encrypted)
            self.assertFalse(os.path.isdir(os.path.join(
                self.t.profile, "profile", "Network", "Cache")))
            for name in ('GPUCache', 'DawnGraphiteCache', 'DawnWebGPUCache',
                         'Shared Dictionary'):
                self.assertFalse(os.path.exists(os.path.join(
                    self.t.profile, 'profile', name)), f'disk cache created: {name}')
        self.t.start(fresh=False)
        self.r = self.t.remote
        page = self.home()
        self.assertIn(f"persist={value}", page.eval("document.cookie"))
        self.assertEqual(page.eval("localStorage.getItem('persist')"), value)
        stored = page.eval("""(async () => {
          const db = await new Promise((resolve, reject) => {
            const r = indexedDB.open('chorium-e2e', 1);
            r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error);
          });
          const value = await new Promise((resolve, reject) => {
            const r = db.transaction('values').objectStore('values').get('key');
            r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error);
          });
          db.close(); return value;
        })()""")
        self.assertEqual(stored, value)
        self.assertEqual(page.eval("(async () => (await (await caches.open("
                                   "'chorium-e2e')).match('/persisted')).text())()"), value)
        self.assertEqual(page.eval("navigator.serviceWorker.getRegistrations()"
                                   ".then(r => r.length)"), 1)
        self.b.open_menu()
        self.r.tap_text("Settings", exact=True)
        label = self.b.settings_row("Clear cookies and site data")
        buttons = [t for t in self.r.texts() if t.text == "Clear"
                   and abs(t.y - label.y) < 25]
        self.assertTrue(buttons, "no Clear button next to site data")
        self.r.tap(*min(buttons, key=lambda t: abs(t.y - label.y)).center)
        self.r.wait_for(lambda: page.eval("document.cookie") == "" and
                        page.eval("localStorage.getItem('persist')") is None,
                        20, "cookies and local storage to clear")
        self.r.wait_for(lambda: page.eval("Promise.all([indexedDB.databases(),"
                        "caches.keys(), navigator.serviceWorker.getRegistrations()])"
                        ".then(lists => lists.every(list => list.length === 0))"),
                        20, "IndexedDB, CacheStorage and service workers to clear")
        page.close()
        self.t.restart()
        self.r = self.t.remote
        page = self.home()
        self.assertNotIn("persist=", page.eval("document.cookie"))
        self.assertIsNone(page.eval("localStorage.getItem('persist')"))

    def test_address_bar_typing(self):
        page = self.home()
        self.assertEqual(page.eval("document.title"), "E2E home")

    def test_typing_in_page_search_box_twice_no_crash(self):
        # The DuckDuckGo crash: tapping the box again after typing.
        page = self.home()
        self.b.tap_element(page, "#q")
        time.sleep(0.5)
        self.r.type("first words")
        time.sleep(0.5)
        self.b.tap_element(page, "#q")
        time.sleep(0.5)
        self.r.type(" more")
        self.r.key(VK_RETURN)
        self.r.wait_for(lambda: page.eval("document.title").startswith(
            "searched:"), 10, "the search to run")
        self.assertIn("first words", page.eval("document.title"))
        self.r.state()  # the app is still there

    def test_tor_switch_and_pill(self):
        self.b.set_tor(False)
        self.r.wait_text("Direct", exact=True)
        self.b.set_tor(True)
        self.r.wait_text("Tor", exact=True)
        self.assertTrue(self.r.state()["tor_enabled"])

    def test_pages_go_through_tor(self):
        if not NETWORK:
            self.skipTest("needs the internet (run.py --network)")
        self.b.set_tor(True)
        page = self.b.go("https://check.torproject.org/",
                         "check.torproject.org")
        self.r.wait_for(lambda: "Congratulations" in (page.eval(
            "document.body && document.body.innerText") or ""), 180,
            "check.torproject.org to say Congratulations")

    def test_pages_think_they_are_online(self):
        page = self.home()
        self.assertTrue(page.eval("navigator.onLine"))

    def test_back_returns_to_previous_page(self):
        page = self.home()
        self.b.tap_element(page, "#link")
        second = self.b.page("/pages/second.html")
        self.assertEqual(second.eval("document.title"), "E2E second")
        if self.t.phone:
            self.r.back()
        else:
            self.r.tap_icon("back")
        self.r.wait_for(lambda: page.eval("document.title") == "E2E home",
                        10, "back to the first page")

    def test_full_screen_hides_the_app(self):
        page = self.home()
        self.b.tap_element(page, "#fs")
        self.r.wait_for(lambda: self.r.state()["fullscreen"], 10,
                        "full screen")
        self.assertFalse(self.r.shown("127.0.0.1"))  # no address bar
        if self.t.phone:
            self.r.back()
        else:
            self.r.key(VK_ESCAPE)
        self.r.wait_for(lambda: not self.r.state()["fullscreen"], 10,
                        "full screen to end")

    def test_dark_theme_reaches_pages(self):
        page = self.home()
        self.b.open_menu()
        self.r.tap_text("Settings", exact=True)
        self.r.tap(*self.b.settings_row("Dark").center)
        self.r.wait_for(lambda: page.eval(
            "matchMedia('(prefers-color-scheme: dark)').matches"), 10,
            "pages to turn dark")
        self.r.tap(*self.b.settings_row("Light").center)
        self.r.wait_for(lambda: not page.eval(
            "matchMedia('(prefers-color-scheme: dark)').matches"), 10,
            "pages to turn light")
        self.b.close_menus()

    def test_new_tab_and_session_restore(self):
        self.home()
        tabs_before = len(cdp.targets(self.t.devtools_port))
        if self.t.phone:
            self.b.open_menu()
            self.r.tap_text("New tab", exact=True)
        else:
            self.r.key(ord("T"), CTRL)
        time.sleep(1)
        self.b.go(SERVER.url("/pages/second.html"), "/pages/second.html")
        self.assertEqual(len(cdp.targets(self.t.devtools_port)),
                         tabs_before + 1)
        self.t.restart()
        self.r = self.t.remote
        # Both saved addresses come back. Loopback is deliberately unreachable
        # through Tor, which resets to on at startup, so check restoration
        # independently of loading these local fixtures through Tor.
        expected = [SERVER.url('/pages/index.html'), SERVER.url('/pages/second.html')]
        self.r.wait_for(lambda: sorted(t['url'] for t in cdp.targets(
            self.t.devtools_port)) == sorted(expected), 30, 'both saved tab addresses')
        self.b.set_tor(False)  # startup deliberately resets Tor to on
        self.b.go(expected[1], '/pages/second.html')
