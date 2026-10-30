#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""One-time setup: fetch Chromium at the pinned version, wire Umbra into it.

  python3 scripts/bootstrap.py --target android     # on Linux (x64)
  python  scripts\\bootstrap.py --target win         # on Windows 10/11 x64

Host requirements (Chromium's, not ours):
  Android: Linux x64, ~120 GB free, 16+ GB RAM (32 GB recommended).
  Windows: Visual Studio 2022 with "Desktop development with C++", the
           Windows 11 SDK and its Debugging Tools; ~120 GB free on NTFS.
Plus Rust (rustup) for the Tor core.

What it does:
  1. depot_tools     -> <work>/depot_tools
  2. fetch Chromium  -> <work>/chromium/src  at CHROMIUM_VERSION
  3. build deps + gclient runhooks
  4. links this repo in as src/umbra
  5. applies the hook edits (scripts/hooks.py)
Re-running is safe; finished steps are skipped or cheap.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IS_WIN = platform.system() == "Windows"
DEPOT_TOOLS_URL = "https://chromium.googlesource.com/chromium/tools/depot_tools.git"


def run(cmd, cwd=None, env=None):
  print("+", " ".join(cmd), flush=True)
  # depot_tools entry points are .bat files on Windows.
  subprocess.check_call(cmd, cwd=cwd, env=env, shell=IS_WIN)


def default_work_dir():
  # Short path on Windows: Chromium hits MAX_PATH limits in deep directories.
  return r"C:\umbra" if IS_WIN else os.path.expanduser("~/umbra-build")


def ensure_depot_tools(work, env):
  dt = os.path.join(work, "depot_tools")
  if not os.path.isdir(dt):
    run(["git", "clone", DEPOT_TOOLS_URL, dt])
  env["PATH"] = dt + os.pathsep + env["PATH"]
  if IS_WIN:
    # Use the locally installed Visual Studio instead of Google's internal one.
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    run(["gclient"], env=env)  # first run installs depot_tools' Python etc.
  return dt


def ensure_gclient_target_os(chromium_dir, target):
  path = os.path.join(chromium_dir, ".gclient")
  with open(path, encoding="utf-8") as f:
    content = f.read()
  if target == "android" and "target_os" not in content:
    with open(path, "a", encoding="utf-8") as f:
      f.write("\ntarget_os = ['android']\n")
    print("added target_os = ['android'] to .gclient")


def link_umbra(src):
  link = os.path.join(src, "umbra")
  if os.path.lexists(link):
    if os.path.realpath(link) != os.path.realpath(ROOT):
      sys.exit(f"{link} exists and is not this repo; move it out of the way")
    return
  if IS_WIN:
    subprocess.check_call(["cmd", "/c", "mklink", "/J", link, ROOT])
  else:
    os.symlink(ROOT, link)
  # Keep Chromium's `git status` clean.
  exclude = os.path.join(src, ".git", "info", "exclude")
  if os.path.isfile(exclude):
    with open(exclude, "a+", encoding="utf-8") as f:
      f.seek(0)
      if "/umbra" not in f.read():
        f.write("\n/umbra\n")
  print(f"linked {link} -> {ROOT}")


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("--target", required=True, choices=["win", "android"])
  ap.add_argument("--work", default=default_work_dir())
  ap.add_argument("--skip-sync", action="store_true",
                  help="don't (re)sync Chromium; just relink and re-apply hooks")
  ap.add_argument("--skip-deps", action="store_true",
                  help="skip install-build-deps.sh (Linux, needs sudo)")
  ap.add_argument("--jobs", type=int, default=8,
                  help="parallel git fetches during sync; googlesource rate-"
                       "limits anonymous clients (HTTP 429) at gclient's "
                       "default of one per CPU")
  args = ap.parse_args()

  if args.target == "win" and not IS_WIN:
    sys.exit("This script bootstraps Windows builds on Windows only. To "
             "cross-compile Windows from Linux with a packaged Windows SDK, "
             "see chorium/docker (Chromium docs/win_cross.md).")
  if args.target == "android" and platform.system() != "Linux":
    sys.exit("Android builds must be bootstrapped on Linux x64.")

  work = os.path.abspath(args.work)
  chromium_dir = os.path.join(work, "chromium")
  src = os.path.join(chromium_dir, "src")
  os.makedirs(chromium_dir, exist_ok=True)
  env = dict(os.environ)
  ensure_depot_tools(work, env)

  dash_platform = "Windows" if args.target == "win" else "Android"
  version = subprocess.check_output(
      [sys.executable, os.path.join(ROOT, "scripts", "chromium_version.py"),
       "--platform", dash_platform], text=True).strip()
  print(f"Chromium version: {version}")

  if not args.skip_sync:
    if not os.path.isfile(os.path.join(chromium_dir, ".gclient")):
      run(["fetch", "--nohooks", "--no-history",
           "android" if args.target == "android" else "chromium"],
          cwd=chromium_dir, env=env)
    ensure_gclient_target_os(chromium_dir, args.target)
    # Fetch only the pinned tag, then sync to its commit hash. `--with_tags`
    # on a --no-history clone negotiates every Chromium tag and can stall for
    # hours, and gclient re-fetches all of origin for any revision that isn't
    # a raw SHA (gclient_scm.py: IsValidRevision(..., sha_only=True)).
    tag = f"refs/tags/{version}"
    run(["git", "fetch", "--depth=1", "--no-tags", "origin", f"+{tag}:{tag}"],
        cwd=src, env=env)
    sha = subprocess.check_output(["git", "rev-parse", f"{tag}^{{commit}}"],
                                  cwd=src, text=True).strip()
    run(["gclient", "sync", "-D", "--no-history", "--nohooks",
         "--jobs", str(args.jobs), "--revision", f"src@{sha}"],
        cwd=chromium_dir, env=env)
    if args.target == "android" and not args.skip_deps:
      run(["sudo", os.path.join(src, "build", "install-build-deps.sh"),
           "--android", "--no-prompt"], env=env)
    run(["gclient", "runhooks"], cwd=chromium_dir, env=env)

  link_umbra(src)
  run([sys.executable, os.path.join(ROOT, "scripts", "hooks.py"), "apply",
       "--src", src, "--platform", args.target])

  if shutil.which("rustup") is None:
    print("\nNOTE: install Rust from https://rustup.rs before building; "
          "the Tor core is built with cargo.")
  print(f"\nReady. Next: python3 scripts/build.py --target {args.target} "
        f"--work {work}")


if __name__ == "__main__":
  main()
