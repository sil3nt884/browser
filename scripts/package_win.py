#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""Copies a Windows build into a portable, self-contained folder (and zip).

  python scripts\\package_win.py --build C:\\umbra\\chromium\\src\\out\\umbra-win-x64-release
  python scripts\\package_win.py --build <out dir> --dest D:\\Umbra --zip D:\\Umbra.zip

The file list comes from Chromium's own installer manifest,
chrome/installer/mini_installer/chrome.release (which the `win-installer`
hook extends with umbra_tor.dll), so the package holds what an installed
Chrome would, minus Google-branded extras. The layout is flat, like the build
dir, which is what Umbra was tested with: chrome.exe runs straight from it.
A zip does not carry the folder's sandbox permission. Unzipped under Program
Files it is inherited; anywhere else, run once:
  icacls <folder> /grant *S-1-15-2-2:(OI)(CI)(RX)
"""

import argparse
import configparser
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# chrome.release sections that apply to a Chromium (non-Google) build.
# Missing files are skipped: several entries only exist in some configs.
SECTIONS = ["GENERAL", "HIDPI", "FFMPEG", "SNAPSHOTBLOB", "DXC"]

# Kept next to chrome.exe in the build dir; not in chrome.release because
# setup.exe generates them, but a portable copy needs them.
EXTRA = ["chrome.VisualElementsManifest.xml", "First Run"]


def release_patterns(src):
  path = os.path.join(src, "chrome", "installer", "mini_installer",
                      "chrome.release")
  cp = configparser.ConfigParser(delimiters=":", allow_no_value=True,
                                 interpolation=None, strict=False)
  cp.optionxform = str  # keep file-name case
  cp.read(path, encoding="utf-8")
  out = []
  for section in SECTIONS:
    if cp.has_section(section):
      out += list(cp[section].keys())
  return out


def package(build, dest, zip_path=None):
  build = os.path.abspath(build)
  src = os.path.dirname(os.path.dirname(build))
  if not os.path.isfile(os.path.join(build, "chrome.exe")):
    sys.exit(f"no chrome.exe in {build}")
  if os.path.exists(dest):
    sys.exit(f"{dest} already exists; remove it or pick another --dest")

  files = []
  for pattern in release_patterns(src) + EXTRA:
    files += [p for p in glob.glob(os.path.join(build, pattern))
              if os.path.isfile(p)]
  if not any(os.path.basename(f) == "umbra_tor.dll" for f in files):
    sys.exit("umbra_tor.dll missing from the build: without it Umbra "
             "fails closed and loads nothing")

  for f in sorted(set(files)):
    rel = os.path.relpath(f, build)
    os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
    shutil.copy2(f, os.path.join(dest, rel))
  print(f"copied {len(set(files))} files to {dest}")

  # Chromium's LPAC sandbox (network service and friends) has to read
  # chrome.exe. The build dir and Program Files grant this; a folder made
  # anywhere else does not ("Sandbox cannot access executable ... (0x5)").
  # Same grant as the build: S-1-15-2-2 = ALL RESTRICTED APPLICATION PACKAGES.
  subprocess.check_call(["icacls", dest, "/grant", "*S-1-15-2-2:(OI)(CI)(RX)",
                         "/q"])

  if zip_path:
    base = os.path.splitext(os.path.abspath(zip_path))[0]
    archive = shutil.make_archive(base, "zip", os.path.dirname(dest),
                                  os.path.basename(dest))
    print(f"zip: {archive}")


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("--build", required=True, help="the out\\... build dir")
  ap.add_argument("--dest", default=os.path.join(ROOT, "dist", "umbra-win-x64"))
  ap.add_argument("--zip", metavar="PATH", help="also write a zip of <dest>")
  args = ap.parse_args()
  package(args.build, os.path.abspath(args.dest), args.zip)


if __name__ == "__main__":
  main()
