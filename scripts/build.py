#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""Builds Umbra for Windows or Android from a bootstrapped checkout.

  python3 scripts/build.py --target android            # -> dist/Umbra-arm64.apk
  python  scripts\\build.py --target win                # -> dist/umbra-win-x64/

Options:
  --cpu      arm64 | x64  (default: arm64 for Android, x64 for Windows)
  --debug    debug build (much slower to run, faster to link)
  --installer  Windows: also build mini_installer.exe
  --skip-tor   reuse the existing prebuilt Tor core
"""

import argparse
import glob
import os
import platform
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IS_WIN = platform.system() == "Windows"
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from bootstrap import default_work_dir  # noqa: E402


def run(cmd, cwd=None, env=None):
  print("+", " ".join(cmd), flush=True)
  subprocess.check_call(cmd, cwd=cwd, env=env, shell=IS_WIN)


def read_args(name):
  with open(os.path.join(ROOT, "build", "args", name), encoding="utf-8") as f:
    return f.read()


def write_args_gn(out_dir, target, cpu, debug):
  text = read_args("common.gn") + "\n" + read_args(f"{target}.gn")
  text += f'\ntarget_cpu = "{cpu}"\n'
  if debug:
    text += "\n# --debug\nis_debug = true\nsymbol_level = 1\n"
  os.makedirs(out_dir, exist_ok=True)
  path = os.path.join(out_dir, "args.gn")
  old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
  if old != text:  # rewriting unchanged args forces a full gn regen
    with open(path, "w", encoding="utf-8") as f:
      f.write(text)


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("--target", required=True, choices=["win", "android"])
  ap.add_argument("--cpu", choices=["arm64", "x64"])
  ap.add_argument("--work", default=default_work_dir())
  ap.add_argument("--debug", action="store_true")
  ap.add_argument("--installer", action="store_true")
  ap.add_argument("--skip-tor", action="store_true")
  args = ap.parse_args()
  cpu = args.cpu or ("arm64" if args.target == "android" else "x64")

  work = os.path.abspath(args.work)
  src = os.path.join(work, "chromium", "src")
  if not os.path.isdir(os.path.join(src, "umbra")):
    sys.exit(f"{src}/umbra not found; run scripts/bootstrap.py first")
  env = dict(os.environ)
  env["PATH"] = os.path.join(work, "depot_tools") + os.pathsep + env["PATH"]
  if IS_WIN:
    env["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"

  # 1. Hooks still in place? (A `gclient sync` can reset touched files.)
  run([sys.executable, os.path.join(ROOT, "scripts", "hooks.py"), "apply",
       "--src", src, "--platform", args.target])

  # 2. Tor core.
  if not args.skip_tor:
    cmd = [sys.executable, os.path.join(ROOT, "scripts", "build_tor_core.py"),
           "--os", args.target, "--cpu", cpu, "--chromium-src", src]
    if args.debug:
      cmd.append("--debug")
    run(cmd)

  # 3. gn + ninja.
  kind = "debug" if args.debug else "release"
  out_rel = os.path.join("out", f"umbra-{args.target}-{cpu}-{kind}")
  out_dir = os.path.join(src, out_rel)
  write_args_gn(out_dir, args.target, cpu, args.debug)
  run(["gn", "gen", out_rel], cwd=src, env=env)

  if args.target == "android":
    targets = ["chrome_public_apk"]
  else:
    targets = ["chrome"] + (["mini_installer"] if args.installer else [])
  run(["autoninja", "-C", out_rel, *targets], cwd=src, env=env)

  # 4. Collect outputs.
  dist = os.path.join(ROOT, "dist")
  os.makedirs(dist, exist_ok=True)
  if args.target == "android":
    apks = glob.glob(os.path.join(out_dir, "apks", "ChromePublic*.apk"))
    if not apks:
      sys.exit("build finished but no APK found in out/.../apks")
    dest = os.path.join(dist, f"Umbra-{cpu}{'-debug' if args.debug else ''}.apk")
    shutil.copy2(apks[0], dest)
    print(f"\nAPK: {dest}\nInstall: adb install -r {dest}")
  else:
    if args.installer:
      inst = os.path.join(out_dir, "mini_installer.exe")
      shutil.copy2(inst, os.path.join(dist, f"umbra-setup-{cpu}.exe"))
    print(f"\nRun it from the build dir: {os.path.join(out_dir, 'chrome.exe')}")


if __name__ == "__main__":
  main()
