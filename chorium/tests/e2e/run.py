# Copyright 2026 The Umbra Authors. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Runs the end-to-end tests against the real app.

  python run.py --platform win --dist C:\\me-tor\\Chorium\\win-x64
  python run.py --platform android [--apk Chorium.apk] [--serial ID]
  options: --network (also check pages through Tor), -k <name filter>

Windows: starts Chorium.exe with --remote-control and its own profile (your
history and settings aren't touched; downloads go to your Downloads folder
and are deleted again; the clipboard is used). Android: the phone over USB
(adb), the app with its own test profile; the phone reaches the test pages
through adb reverse. Python 3 standard library only."""

import argparse
import os
import sys
import shutil
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from chorium_e2e import server as server_mod  # noqa: E402
from chorium_e2e import targets  # noqa: E402
import test_chorium  # noqa: E402


class ScreenshotResult(unittest.TextTestResult):
    """Keeps a picture of the screen for every failed test (e2e-failures/)."""

    def _picture(self, test):
        os.makedirs("e2e-failures", exist_ok=True)
        log = getattr(test_chorium.TARGET, "log_path", None)
        if log and os.path.isfile(log):
            shutil.copyfile(log, os.path.join("e2e-failures", test.id() + ".log"))
        try:
            os.makedirs("e2e-failures", exist_ok=True)
            test_chorium.TARGET.save_screenshot(
                os.path.join("e2e-failures", test.id()))
        except Exception as e:  # the app may be gone
            print(f"    (no screenshot: {e})")

    def addFailure(self, test, err):
        self._picture(test)
        super().addFailure(test, err)

    def addError(self, test, err):
        self._picture(test)
        super().addError(test, err)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", choices=["win", "android"], required=True)
    ap.add_argument("--dist", help="folder with Chorium.exe (Windows)")
    ap.add_argument("--apk", help="install this APK first (Android)")
    ap.add_argument("--serial", help="adb device serial (Android)")
    ap.add_argument("--network", action="store_true",
                    help="also run tests that need the internet (Tor)")
    ap.add_argument("-k", dest="filter", help="only tests with this in the name")
    args = ap.parse_args()

    srv = server_mod.Server()
    if args.platform == "win":
        if not args.dist:
            ap.error("--dist is needed on Windows")
        target = targets.WindowsTarget(args.dist)
    else:
        target = targets.AndroidTarget(args.apk, args.serial)
        target.reverse(server_mod.PORT)
    test_chorium.TARGET = target
    test_chorium.SERVER = srv
    test_chorium.NETWORK = args.network

    loader = unittest.TestLoader()
    if args.filter:
        loader.testNamePatterns = [f"*{args.filter}*"]
    suite = loader.loadTestsFromModule(test_chorium)
    try:
        result = unittest.TextTestRunner(verbosity=2,
                                         resultclass=ScreenshotResult).run(suite)
    finally:
        target.stop()
        target.cleanup()
        srv.stop()
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
