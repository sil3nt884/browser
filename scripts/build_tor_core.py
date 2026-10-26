#!/usr/bin/env python3
# Copyright 2026 The Umbra Authors.
"""Builds the Rust Tor core (tor_core/) and drops the shared library into
prebuilt/<target_os>/<target_cpu>/, where //umbra/tor_core:runtime picks it up.

  build_tor_core.py --os win     --cpu x64      # on Windows (MSVC)
  build_tor_core.py --os android --cpu arm64    # on Linux, needs the NDK
  build_tor_core.py --os linux   --cpu x64      # dev builds

For Android the NDK is taken from, in order: --ndk, $ANDROID_NDK_HOME, or the
one Chromium ships at <chromium src>/third_party/android_toolchain/ndk.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CRATE = os.path.join(ROOT, "tor_core")

# (gn os, gn cpu) -> (rust triple, cargo-ndk ABI or None, output file name)
TARGETS = {
    ("win", "x64"): ("x86_64-pc-windows-msvc", None, "umbra_tor.dll"),
    ("win", "arm64"): ("aarch64-pc-windows-msvc", None, "umbra_tor.dll"),
    ("android", "arm64"): ("aarch64-linux-android", "arm64-v8a", "libumbra_tor.so"),
    ("android", "x64"): ("x86_64-linux-android", "x86_64", "libumbra_tor.so"),
    ("linux", "x64"): ("x86_64-unknown-linux-gnu", None, "libumbra_tor.so"),
    ("linux", "arm64"): ("aarch64-unknown-linux-gnu", None, "libumbra_tor.so"),
    ("mac", "arm64"): ("aarch64-apple-darwin", None, "libumbra_tor.dylib"),
    ("mac", "x64"): ("x86_64-apple-darwin", None, "libumbra_tor.dylib"),
}

# Chromium's minSdkVersion is higher than this; 26 just keeps the .so loadable
# on anything Chromium supports.
ANDROID_API = "26"


def run(cmd, **kw):
  print("+", " ".join(cmd), flush=True)
  subprocess.check_call(cmd, **kw)


def find_ndk(args):
  candidates = [args.ndk, os.environ.get("ANDROID_NDK_HOME")]
  if args.chromium_src:
    candidates.append(os.path.join(args.chromium_src, "third_party",
                                   "android_toolchain", "ndk"))
  for c in candidates:
    if c and os.path.isdir(c):
      return os.path.abspath(c)
  sys.exit("Android NDK not found: pass --ndk, set ANDROID_NDK_HOME, or pass "
           "--chromium-src so the NDK bundled with Chromium is used.")


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("--os", required=True, choices=sorted({k[0] for k in TARGETS}))
  ap.add_argument("--cpu", required=True, choices=sorted({k[1] for k in TARGETS}))
  ap.add_argument("--ndk", help="Android NDK root")
  ap.add_argument("--chromium-src", help="Chromium src/ (to reuse its NDK)")
  ap.add_argument("--debug", action="store_true")
  args = ap.parse_args()

  key = (args.os, args.cpu)
  if key not in TARGETS:
    sys.exit(f"unsupported combination: {key}")
  triple, ndk_abi, libname = TARGETS[key]

  if args.os == "win" and platform.system() != "Windows":
    sys.exit("Windows builds of the Tor core need a Windows host with MSVC.")

  run(["rustup", "target", "add", triple])
  profile = "debug" if args.debug else "release"
  cargo_flags = [] if args.debug else ["--release"]

  if ndk_abi:
    env = dict(os.environ, ANDROID_NDK_HOME=find_ndk(args))
    if shutil.which("cargo-ndk") is None:
      run(["cargo", "install", "cargo-ndk", "--locked"])
    run(["cargo", "ndk", "-t", ndk_abi, "--platform", ANDROID_API, "build",
         *cargo_flags], cwd=CRATE, env=env)
  else:
    run(["cargo", "build", "--target", triple, *cargo_flags], cwd=CRATE)

  built = os.path.join(CRATE, "target", triple, profile, libname)
  if not os.path.isfile(built):
    sys.exit(f"build finished but {built} is missing")
  dest_dir = os.path.join(ROOT, "prebuilt", args.os, args.cpu)
  os.makedirs(dest_dir, exist_ok=True)
  dest = os.path.join(dest_dir, libname)
  shutil.copy2(built, dest)
  print(f"\nTor core -> {os.path.relpath(dest, ROOT)} "
        f"({os.path.getsize(dest) / 1e6:.1f} MB)")


if __name__ == "__main__":
  main()
