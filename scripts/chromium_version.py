#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""Prints the Chromium version Umbra builds against.

CHROMIUM_VERSION at the repo root holds either a version ("146.0.7680.80") or
"auto". "auto" asks chromiumdash for the current Stable release on the given
platform. Use --pin to write the resolved version back, so every machine and CI
runner builds the same tree.

  chromium_version.py              # print the version to use
  chromium_version.py --pin        # resolve "auto" and record it
"""

import argparse
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(ROOT, "CHROMIUM_VERSION")
DASH = ("https://chromiumdash.appspot.com/fetch_releases"
        "?channel=Stable&platform={platform}&num=1")


def latest_stable(platform: str) -> str:
  with urllib.request.urlopen(DASH.format(platform=platform), timeout=30) as r:
    data = json.load(r)
  if not data or "version" not in data[0]:
    sys.exit(f"unexpected chromiumdash response: {data!r}")
  return data[0]["version"]


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("--pin", action="store_true")
  ap.add_argument("--platform", default="Android",
                  help="chromiumdash platform name (Android, Windows, Linux)")
  args = ap.parse_args()

  with open(VERSION_FILE, encoding="utf-8") as f:
    wanted = f.read().strip()
  version = latest_stable(args.platform) if wanted == "auto" else wanted
  if args.pin and wanted == "auto":
    with open(VERSION_FILE, "w", encoding="utf-8") as f:
      f.write(version + "\n")
    print(f"pinned {version}", file=sys.stderr)
  print(version)


if __name__ == "__main__":
  main()
