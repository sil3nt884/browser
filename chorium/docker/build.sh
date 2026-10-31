#!/bin/bash
# Copyright 2026 The Umbra Authors.
#
# Builds Chorium for one target inside the chorium-build image:
#
#   docker run --rm --cpus=10 --memory=7g -v chorium-work:/work \
#     -v C:/me-tor/umbra:/umbra:ro \
#     -v C:/me-tor/chorium-winsdk/vs_files:/work/chromium/src/third_party/depot_tools/win_toolchain/vs_files \
#     -e WIN_TOOLCHAIN_HASH=<zip name> \
#     chorium-build:157 bash /umbra/chorium/docker/build.sh win [ninja targets]
#
# Android (arm64) needs no SDK mount: build.sh android -> apks/Chorium.apk.
# Before it: the Tor library (prebuilt/android/arm64, build_tor_core.py),
# Wasmtime's Android C API and the icon font in /work/wasm-tools, and the
# app (OUT=/work/wasm/out build_wasm.sh app browser).
#
# The Windows SDK only works on a case-insensitive file system. Chromium
# normally mounts one with FUSE (ciopfs), which needs a privileged container;
# instead the SDK lives on NTFS (case-insensitive) and is bind-mounted over
# vs_files, which makes build/vs_toolchain.py skip ciopfs.
#
# Output: /work/chromium/src/out/chorium-<target>/ (chorium.dll etc.).
# Only targets Chorium depends on are loaded (gn --root-target), so nothing
# from //chrome is configured or compiled.
set -euo pipefail

TARGET=${1:-win}
shift || true
case $TARGET in
  win)
    # Chorium.exe (with Chromium's sandbox), chorium.dll, chorium_test.exe.
    ROOT_TARGET=//umbra/chorium/runtime/win:win
    CPU=${CPU:-x64}
    ;;
  android)
    # The whole runtime: Chorium.apk (engine, runtime, Wasmtime, the app).
    ROOT_TARGET=//umbra/chorium/runtime/android:chorium_apk
    CPU=${CPU:-arm64}
    ;;
  *)
    echo "build.sh: unknown target $TARGET (win, android)" >&2
    exit 1
    ;;
esac
# Extra arguments go to ninja. If none of them names a target (only flags
# such as -k 0), build the default one: plain ninja would build everything
# in the build files, host tools included.
NINJA_ARGS=("$@")
has_target=false
for arg in "$@"; do
  [[ $arg == */* || $arg == *:* ]] && has_target=true
done
$has_target || NINJA_ARGS+=("${ROOT_TARGET#//}")
JOBS=${JOBS:-8}
SRC=/work/chromium/src
OUT=out/chorium-$TARGET

source /umbra/chorium/docker/env.sh

# //umbra inside the checkout points at the repo mounted from Windows.
if [[ ! -L $SRC/umbra ]]; then
  ln -s /umbra "$SRC/umbra"
  grep -qx /umbra "$SRC/.git/info/exclude" || echo /umbra >> "$SRC/.git/info/exclude"
fi

mkdir -p "$SRC/$OUT"
# Do not remove another live build's ninja lock. flock is released even
# after a killed container; the lock file itself can safely remain.
exec 9>"$SRC/.chorium-build.lock"
flock -n 9 || { echo "A Chorium build is already running in this volume" >&2; exit 1; }
rm -f "$SRC/$OUT/.ninja_lock"
if [[ $TARGET == win && -n ${WIN_TOOLCHAIN_HASH:-} ]]; then
  python3 /umbra/scripts/hooks.py apply --src "$SRC" --platform win \
    --only packaged-vs-version
fi
{
  cat /umbra/build/args/common.gn
  echo
  echo "# Chorium ($TARGET)"
  echo "target_os = \"$TARGET\""
  echo "target_cpu = \"$CPU\""
  # Ninja, not Siso: //umbra is a symlink to a folder outside the checkout.
  echo 'use_siso = false'
  # Not an arg: makes a new root target count as changed args (below).
  echo "# root target: $ROOT_TARGET"
} > "$SRC/$OUT/args.gn.new"

cd "$SRC"
# gn gen takes minutes; ninja re-runs it by itself when a BUILD.gn changes,
# so only run it when the args changed or the out dir is new.
if ! cmp -s "$OUT/args.gn.new" "$OUT/args.gn" || [[ ! -f $OUT/build.ninja ]]; then
  mv "$OUT/args.gn.new" "$OUT/args.gn"
  gn gen "$OUT" --root-target="$ROOT_TARGET"
else
  rm "$OUT/args.gn.new"
fi
echo "BUILD START $(date -u +%FT%TZ)"
# Share CPU with desktop work, but do not starve builds behind all other I/O.
# BUILD_NICE=19 and BUILD_IO_CLASS=3 restore the old idle-only behavior.
BUILD_NICE=${BUILD_NICE:-5}
BUILD_IO_CLASS=${BUILD_IO_CLASS:-2}
BUILD_IO_PRIORITY=${BUILD_IO_PRIORITY:-7}
IO_ARGS=(-c "$BUILD_IO_CLASS")
[[ $BUILD_IO_CLASS != 3 ]] && IO_ARGS+=(-n "$BUILD_IO_PRIORITY")
nice -n "$BUILD_NICE" ionice "${IO_ARGS[@]}" \
  autoninja -C "$OUT" -j "$JOBS" "${NINJA_ARGS[@]}"
echo "BUILD DONE $(date -u +%FT%TZ)"
if [[ -f $OUT/chorium.dll ]]; then
  # umbra_tor.dll comes from prebuilt/; cross builds ship snapshot_blob.bin.
  ls -la "$OUT"/Chorium.exe "$OUT"/chorium.dll "$OUT"/chorium.pak \
         "$OUT"/chorium_test.exe \
         "$OUT"/icudtl.dat "$OUT"/snapshot_blob.bin
fi
if [[ -f $OUT/apks/Chorium.apk ]]; then
  ls -la "$OUT"/apks/Chorium.apk
fi
