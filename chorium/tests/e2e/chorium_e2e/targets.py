# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Where the app runs: Chorium.exe on this PC, or Chorium.apk on a phone
over adb. Both give the tests the same things: the remote control, DevTools,
the Downloads folder, and platform checks (sandbox, refresh rate)."""

import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from .remote import Remote

REMOTE_PORT = 9334
DEVTOOLS_PORT = 9335


class Target:
    phone = False
    name = "?"

    def start(self, fresh=True):
        raise NotImplementedError

    def stop(self):
        raise NotImplementedError

    def restart(self):
        """Stops the app and starts it again with the same profile."""
        self.stop()
        self.start(fresh=False)


# --- Windows ------------------------------------------------------------------

if sys.platform == "win32":
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    user32.GetClipboardData.restype = wintypes.HANDLE
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [wintypes.HANDLE]
    kernel32.GlobalUnlock.argtypes = [wintypes.HANDLE]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    user32.FindWindowExW.restype = wintypes.HWND
    user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND,
                                     wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.GetDlgItem.restype = wintypes.HWND
    user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                                    wintypes.WPARAM, wintypes.LPARAM]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                                    wintypes.WPARAM, wintypes.LPARAM]
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND,
                                     wintypes.LPARAM)


class WindowsTarget(Target):
    name = "windows"

    def __init__(self, dist):
        self.dist = os.path.abspath(dist)
        self.exe = os.path.join(self.dist, "Chorium.exe")
        if not os.path.isfile(self.exe):
            raise SystemExit(f"no Chorium.exe in {self.dist}")
        self.profile = tempfile.mkdtemp(prefix="chorium-e2e-")
        self.proc = None
        self.log = None
        self.log_path = os.path.join(self.profile, "host.log")
        self.remote = None
        self.devtools_port = DEVTOOLS_PORT
        self.downloads = os.path.join(os.environ["USERPROFILE"], "Downloads")

    def start(self, fresh=True):
        if fresh:
            shutil.rmtree(self.profile, ignore_errors=True)
            os.makedirs(self.profile)
        self.log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [self.exe, "--remote-control", f"--data-dir={self.profile}"],
            cwd=self.dist,
            stdout=self.log, stderr=subprocess.STDOUT)
        self.remote = Remote(REMOTE_PORT)

    def stop(self):
        """Closes the window as a person would (the app saves its session)."""
        if self.remote:
            self.remote.close()
            self.remote = None
        if not self.proc:
            return
        hwnd = self.main_window()
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.proc.pid)],
                           capture_output=True)
        self.proc = None
        if self.log:
            self.log.close()
            self.log = None
        time.sleep(1)  # helpers exit

    def cleanup(self):
        shutil.rmtree(self.profile, ignore_errors=True)

    def main_window(self):
        found = []

        def each(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            name = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, name, 64)
            if pid.value == self.proc.pid and name.value == "ChoriumHost":
                found.append(hwnd)
            return True

        user32.EnumWindows(WNDENUMPROC(each), 0)
        return found[0] if found else None

    # Files -----------------------------------------------------------------

    def download_path(self, name):
        return os.path.join(self.downloads, name)

    def read_download(self, name):
        try:
            with open(self.download_path(name), "rb") as f:
                return f.read()
        except OSError:
            return None

    def delete_download(self, name):
        try:
            os.remove(self.download_path(name))
        except OSError:
            pass

    def upload_file(self, name, data):
        """A file the person could pick: its path as the picker gives it."""
        path = os.path.join(self.profile, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    # The system's Open dialog --------------------------------------------

    def file_dialog(self, timeout=15):
        """The Open/Save dialog the app showed, or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            found = []

            def each(hwnd, _):
                pid = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                name = ctypes.create_unicode_buffer(64)
                user32.GetClassNameW(hwnd, name, 64)
                if (pid.value == self.proc.pid and name.value == "#32770"
                        and user32.IsWindowVisible(hwnd)):
                    found.append(hwnd)
                return True

            user32.EnumWindows(WNDENUMPROC(each), 0)
            if found:
                return found[0]
            time.sleep(0.2)
        return None

    def answer_file_dialog(self, paths):
        """Types the file names into the real dialog and presses Open, or
        Cancel when |paths| is empty."""
        dialog = self.file_dialog()
        if not dialog:
            raise AssertionError("the system's file dialog didn't open")
        if not paths:
            user32.PostMessageW(dialog, 0x0111, 2, 0)  # WM_COMMAND IDCANCEL
            return
        text = (paths[0] if len(paths) == 1 else
                " ".join(f'"{p}"' for p in paths))
        # File name box: ComboBoxEx32 (id 1148) > ComboBox > Edit; the save
        # dialog has a plain Edit (id 1001) in some layouts.
        edit = None
        combo = user32.GetDlgItem(dialog, 1148)
        if combo:
            inner = user32.FindWindowExW(combo, None, "ComboBox", None)
            edit = user32.FindWindowExW(inner or combo, None, "Edit", None)
        if not edit:
            edit = user32.GetDlgItem(dialog, 1001)
        if not edit:
            raise AssertionError("no file name box in the dialog")
        buf = ctypes.create_unicode_buffer(text)
        user32.SendMessageW(edit, 0x000C, 0, ctypes.addressof(buf))  # SETTEXT
        user32.PostMessageW(dialog, 0x0111, 1, 0)  # WM_COMMAND IDOK

    # Clipboard ---------------------------------------------------------------

    def clipboard_text(self):
        for _ in range(20):
            if user32.OpenClipboard(None):
                break
            time.sleep(0.05)
        else:
            return None
        try:
            h = user32.GetClipboardData(13)  # CF_UNICODETEXT
            if not h:
                return ""
            p = kernel32.GlobalLock(h)
            text = ctypes.wstring_at(p)
            kernel32.GlobalUnlock(h)
            return text
        finally:
            user32.CloseClipboard()

    def clipboard_has_image(self):
        return bool(user32.IsClipboardFormatAvailable(8))  # CF_DIB

    # Platform checks -------------------------------------------------------

    def display_hz(self):
        class DEVMODEW(ctypes.Structure):
            _fields_ = [("name", ctypes.c_wchar * 32), ("spec", ctypes.c_ushort),
                        ("driver", ctypes.c_ushort), ("size", ctypes.c_ushort),
                        ("extra", ctypes.c_ushort), ("fields", ctypes.c_ulong),
                        ("union", ctypes.c_byte * 16),
                        ("color", ctypes.c_short), ("duplex", ctypes.c_short),
                        ("yres", ctypes.c_short), ("tt", ctypes.c_short),
                        ("collate", ctypes.c_short),
                        ("form", ctypes.c_wchar * 32),
                        ("logpixels", ctypes.c_ushort),
                        ("bpp", ctypes.c_ulong), ("width", ctypes.c_ulong),
                        ("height", ctypes.c_ulong), ("flags", ctypes.c_ulong),
                        ("frequency", ctypes.c_ulong)]
        mode = DEVMODEW()
        mode.size = ctypes.sizeof(DEVMODEW)
        user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode))
        return mode.frequency or 60

    def helper_processes(self):
        """Chorium.exe helpers: [(pid, type)] from their command lines."""
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='Chorium.exe'\" | "
             "Select-Object ProcessId,ParentProcessId,CommandLine | "
             "ConvertTo-Json"], capture_output=True, text=True).stdout
        rows = json.loads(out or "[]")
        rows = rows if isinstance(rows, list) else [rows]
        helpers = []
        for r in rows:
            if r.get("ParentProcessId") != self.proc.pid:
                continue
            m = re.search(r"--type=([\w-]+)", r.get("CommandLine") or "")
            if m:
                helpers.append((r["ProcessId"], m.group(1)))
        return helpers

    def integrity_level(self, pid):
        """The process token's integrity RID (0 untrusted, 0x1000 low,
        0x2000 medium), or None if it can't be read."""
        h = kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFO
        if not h:
            return None
        token = wintypes.HANDLE()
        try:
            if not advapi32.OpenProcessToken(h, 0x0008, ctypes.byref(token)):
                return None
            size = wintypes.DWORD()
            advapi32.GetTokenInformation(token, 25, None, 0, ctypes.byref(size))
            buf = ctypes.create_string_buffer(size.value)
            if not advapi32.GetTokenInformation(token, 25, buf, size,
                                                ctypes.byref(size)):
                return None
            sid = ctypes.c_void_p.from_buffer(buf).value
            advapi32.GetSidSubAuthorityCount.restype = ctypes.POINTER(
                ctypes.c_ubyte)
            advapi32.GetSidSubAuthority.restype = ctypes.POINTER(wintypes.DWORD)
            count = advapi32.GetSidSubAuthorityCount(ctypes.c_void_p(sid))[0]
            return advapi32.GetSidSubAuthority(ctypes.c_void_p(sid),
                                               count - 1)[0]
        finally:
            if token:
                kernel32.CloseHandle(token)
            kernel32.CloseHandle(h)

    def in_job(self, pid):
        h = kernel32.OpenProcess(0x1000, False, pid)
        if not h:
            return None
        result = wintypes.BOOL()
        kernel32.IsProcessInJob(h, None, ctypes.byref(result))
        kernel32.CloseHandle(h)
        return bool(result.value)

    def win32k_disabled(self, pid):
        h = kernel32.OpenProcess(0x1000, False, pid)
        if not h:
            return False
        flags = wintypes.DWORD()
        try:
            return bool(kernel32.GetProcessMitigationPolicy(
                h, 4, ctypes.byref(flags), ctypes.sizeof(flags)) and flags.value & 1)
        finally:
            kernel32.CloseHandle(h)

    def save_screenshot(self, path):
        fmt, data = self.remote.screenshot()
        with open(path + "." + fmt, "wb") as f:
            f.write(data)


# --- Android ------------------------------------------------------------------

PACKAGE = "org.umbra.chorium"
ACTIVITY = PACKAGE + "/org.umbra.chorium.runtime.ChoriumActivity"
TEST_PROFILE = "e2e"


class AndroidTarget(Target):
    name = "android"
    phone = True

    def __init__(self, apk=None, serial=None, adb=None, server_port=None):
        self.adb_path = adb or os.environ.get(
            "CHORIUM_ADB", r"C:\me-tor\chorium-tools\platform-tools\adb.exe")
        self.env = dict(os.environ)
        self.env["ADB_SERVER_PORT"] = str(
            server_port or os.environ.get("ADB_SERVER_PORT", "5038"))
        self.serial = serial
        self.remote = None
        self.devtools_port = DEVTOOLS_PORT
        self.downloads = "/sdcard/Download"
        if apk:
            print(f"installing {apk} (accept the phone's prompt if it asks)")
            self.adb("install", "-r", apk, timeout=300)

    def adb(self, *args, timeout=60, check=True):
        cmd = [self.adb_path, "-P", self.env["ADB_SERVER_PORT"]] + \
            (["-s", self.serial] if self.serial else []) + \
            list(args)
        r = subprocess.run(cmd, capture_output=True, env=self.env,
                           timeout=timeout)
        if check and r.returncode != 0:
            raise RuntimeError(f"adb {' '.join(args)}: "
                               f"{r.stderr.decode(errors='replace')}")
        return r.stdout.decode(errors="replace")

    def shell(self, command, **kw):
        return self.adb("shell", command, **kw)

    def start(self, fresh=True):
        self.shell(f"am force-stop {PACKAGE}")
        self.adb("forward", f"tcp:{REMOTE_PORT}", "localabstract:chorium_remote")
        self.adb("forward", f"tcp:{DEVTOOLS_PORT}",
                 "localabstract:chorium_devtools_remote")
        self.shell(f"am start -W -n {ACTIVITY} --ez remote_control true "
                   f"--es data_dir {TEST_PROFILE} "
                   f"--ez data_dir_fresh {'true' if fresh else 'false'}")
        self.remote = Remote(REMOTE_PORT, connect_timeout=90)

    def reverse(self, port):
        """The phone's 127.0.0.1:port reaches this PC's (the test server)."""
        self.adb("reverse", f"tcp:{port}", f"tcp:{port}")

    def stop(self):
        if self.remote:
            self.remote.close()
            self.remote = None
        self.shell(f"am force-stop {PACKAGE}")

    def cleanup(self):
        pass  # the next fresh start clears the test profile

    def download_path(self, name):
        return f"{self.downloads}/{name}"

    def read_download(self, name):
        path = self.download_path(name)
        if "No such file" in self.shell(f"ls {path} 2>&1", check=False):
            return None
        local = os.path.join(tempfile.gettempdir(), "chorium-e2e-" + name)
        self.adb("pull", path, local, check=False)
        try:
            with open(local, "rb") as f:
                return f.read()
        except OSError:
            return None
        finally:
            try:
                os.remove(local)
            except OSError:
                pass

    def delete_download(self, name):
        self.shell(f"rm -f {self.download_path(name)}", check=False)

    def top_activity(self):
        out = self.shell("dumpsys activity activities", check=False)
        m = re.search(r"(?:mResumedActivity|topResumedActivity)[^\n]*?"
                      r"\{[^}]*\s([\w.]+/[\w.$]+)", out)
        return m.group(1) if m else ""

    def clipboard_text(self):
        return None  # Android keeps the clipboard from adb

    def display_hz(self):
        out = self.shell("dumpsys display", check=False)
        rates = [float(r) for r in re.findall(r"refreshRate=([\d.]+)", out)]
        return max(rates) if rates else 60.0

    def renderer_users(self):
        """The users page (sandboxed) processes run as."""
        out = self.shell("ps -A -o USER,NAME", check=False)
        return [line.split()[0] for line in out.splitlines()
                if "sandboxed_process" in line]

    def save_screenshot(self, path):
        data = subprocess.run(
            [self.adb_path, "-P", self.env["ADB_SERVER_PORT"]] +
            (["-s", self.serial] if self.serial else []) +
            ["exec-out", "screencap", "-p"], capture_output=True,
            env=self.env).stdout
        with open(path + ".png", "wb") as f:
            f.write(data)
