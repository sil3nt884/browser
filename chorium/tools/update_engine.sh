#!/usr/bin/env bash
# Copyright 2026 The Umbra Authors.
#
# Keeps Chorium's engine on the current stable Chromium. Runs on the Windows
# host (Git Bash) and drives the Docker build (chorium/docker).
#
#   update_engine.sh check              current pin vs. Chrome stable
#                                       (exit 0 up to date, 10 update available)
#   update_engine.sh update [VERSION]   move to VERSION (default: latest stable),
#                                       rebuild Windows and Android, test,
#                                       package a release
#   update_engine.sh package            package the current builds
#
# The pin is ARG CHROMIUM_TAG in chorium/docker/Dockerfile (the one place the
# version lives). An update edits it; commit that change with any engine
# fixes the new version needed.
#
# Environment: OUT_ROOT (default C:/me-tor/chorium-out), WINSDK_DIR (default
# C:/me-tor/chorium-winsdk/vs_files), WIN_TOOLCHAIN_HASH (418a2884d2),
# WIN_SDK_VERSION (10.0.28000.0: the SDK inside that package), JOBS (6),
# CPUS (18), MEMORY (10g).
set -euo pipefail
export MSYS_NO_PATHCONV=1

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
REPO_DOCKER=$(cygpath -m "$REPO" 2>/dev/null || echo "$REPO")
DOCKERFILE=$REPO/chorium/docker/Dockerfile
OUT_ROOT=${OUT_ROOT:-C:/me-tor/chorium-out}
WINSDK_DIR=${WINSDK_DIR:-C:/me-tor/chorium-winsdk/vs_files}
WIN_TOOLCHAIN_HASH=${WIN_TOOLCHAIN_HASH:-418a2884d2}
WIN_SDK_VERSION=${WIN_SDK_VERSION:-10.0.28000.0}
JOBS=${JOBS:-6}
CPUS=${CPUS:-18}
MEMORY=${MEMORY:-10g}
VOLUME=chorium-work
SRC=/work/chromium/src

say() { printf '\n== %s\n' "$*"; }
die() { printf 'update_engine: %s\n' "$*" >&2; exit 1; }

pinned_version() {
  sed -n 's/^ARG CHROMIUM_TAG=//p' "$DOCKERFILE"
}

image_for() { echo "chorium-build:${1%%.*}"; }

latest_stable() {
  curl -fsS "https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions?pageSize=1" |
    python -c "import json,sys; print(json.load(sys.stdin)['versions'][0]['version'])"
}

# True if VERSION is newer than the pin (dotted numeric compare).
newer() {
  python -c "import sys; a,b=[tuple(map(int,v.split('.'))) for v in sys.argv[1:3]]; sys.exit(0 if a>b else 1)" "$1" "$2"
}

in_container() {  # in_container IMAGE ARGS... -- runs a build container
  local image=$1
  shift
  docker run --rm --cpus="$CPUS" --memory="$MEMORY" --memory-swap="$MEMORY" \
    -v "$VOLUME:/work" -v "$REPO_DOCKER:/umbra:ro" \
    -v "$WINSDK_DIR:$SRC/third_party/depot_tools/win_toolchain/vs_files" \
    -e WIN_TOOLCHAIN_HASH="$WIN_TOOLCHAIN_HASH" -e JOBS="$JOBS" \
    -e CHORIUM_CPU_COUNT=4 "$image" "$@"
}

require_idle() {
  if docker ps --format '{{.Names}}' | grep -q '^chorium-'; then
    die "a chorium build container is running; wait for it to finish"
  fi
}

cmd_check() {
  local pinned latest
  pinned=$(pinned_version)
  latest=$(latest_stable)
  echo "pinned:  $pinned"
  echo "stable:  $latest"
  if newer "$latest" "$pinned"; then
    echo "update available: update_engine.sh update $latest"
    exit 10
  fi
  echo "up to date"
}

# The Windows SDK package must match what the new Chromium asks for.
check_winsdk() {
  local image=$1 want
  want=$(docker run --rm -v "$VOLUME:/work" "$image" \
    sed -n "s/^SDK_VERSION = '\(.*\)'/\1/p" "$SRC/build/vs_toolchain.py")
  if [[ "$want.0" != "$WIN_SDK_VERSION" && "$want" != "$WIN_SDK_VERSION" ]]; then
    die "Chromium now wants Windows SDK $want; the package has $WIN_SDK_VERSION.
Package that SDK on Windows (chorium/README.md, step 2), extract it into
$WINSDK_DIR/<hash>, then rerun with WIN_TOOLCHAIN_HASH=<hash> WIN_SDK_VERSION=$want."
  fi
  echo "Windows SDK $want matches the package"
}

copy_windows() {  # copy_windows IMAGE DEST
  local image=$1 dest=$2
  mkdir -p "$dest/apps"
  docker run --rm -v "$VOLUME:/work" -v "$dest:/dest" "$image" bash -c "
    set -e; cd $SRC/out/chorium-win
    cp chorium.dll chorium.dll.lib chorium_test.exe chorium.pak icudtl.dat \
       snapshot_blob.bin libEGL.dll libGLESv2.dll d3dcompiler_47.dll \
       vk_swiftshader.dll vk_swiftshader_icd.json vulkan-1.dll dxcompiler.dll \
       /dest/
    cp /work/wasm/out/browser.wasm /dest/apps/"
  cp "$REPO/prebuilt/win/x64/umbra_tor.dll" "$dest/"
  cp "$REPO/chorium/interfaces/native/chorium.h" "$dest/"
  # The Windows runtime links chorium.dll.lib from the same folder.
  cmd //c "$(cygpath -w "$REPO/chorium/runtime/win/build.cmd")" "$(cygpath -w "$dest")" |
    grep -E "built|error" || die "Chorium.exe didn't build"
}

copy_android() {  # copy_android IMAGE DEST
  local image=$1 dest=$2
  mkdir -p "$dest"
  docker run --rm -v "$VOLUME:/work" -v "$dest:/dest" "$image" \
    cp "$SRC/out/chorium-android/apks/Chorium.apk" /dest/
}

# manifest.json: what this build is, so an app can tell which engine it has.
write_manifest() {  # write_manifest IMAGE DEST TARGET
  local image=$1 dest=$2 target=$3 out args_hash abi
  out=$([[ $target == windows-x64 ]] && echo chorium-win || echo chorium-android)
  args_hash=$(docker run --rm -v "$VOLUME:/work" "$image" \
    sha256sum "$SRC/out/$out/args.gn" | cut -c1-16)
  abi=$(sed -n 's/^#define CHORIUM_ABI_VERSION \([0-9]*\)/\1/p' \
    "$REPO/chorium/interfaces/native/chorium.h")
  python - "$dest/manifest.json" <<EOF
import json, sys, datetime, hashlib, pathlib
dest = pathlib.Path(sys.argv[1])
files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()[:16]
         for p in sorted(dest.parent.rglob("*")) if p.is_file() and p.name != "manifest.json"}
json.dump({
  "chromiumVersion": "$(pinned_version)",
  "chromiumAbi": int("$abi"),
  "contracts": {"umbra:chorium": "$(sed -n 's/^package umbra:chorium@\(.*\);/\1/p' "$REPO/chorium/interfaces/chorium/chorium.wit")",
                "umbra:browser": "$(sed -n 's/^package umbra:browser@\(.*\);/\1/p' "$REPO/chorium/interfaces/browser/browser.wit")"},
  "target": "$target",
  "gnArgsSha256": "$args_hash",
  "commit": "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)",
  "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
  "files": files,
}, open(dest, "w"), indent=2)
EOF
}

# End-to-end check: a page through Tor, rendered and saved by the engine.
test_windows() {  # test_windows DIR
  local dir=$1 log
  say "test: check.torproject.org through Tor"
  log=$(cd "$dir" && timeout 240 ./chorium_test.exe 2>&1) || true
  rm -f "$dir/chorium_test.bmp"
  if ! grep -q "Congratulations" <<<"$log"; then
    echo "$log" | grep -v "^\s" | tail -20
    die "engine test failed (see above)"
  fi
  echo "ok: the page loaded through Tor"
}

cmd_package() {
  local version image dest
  version=$(pinned_version)
  image=$(image_for "$version")
  dest="$OUT_ROOT/releases/$version"
  say "package $version -> $dest"
  copy_windows "$image" "$dest/windows-x64"
  write_manifest "$image" "$dest/windows-x64" windows-x64
  if docker run --rm -v "$VOLUME:/work" "$image" test -f "$SRC/out/chorium-android/apks/Chorium.apk"; then
    copy_android "$image" "$dest/android-arm64"
    write_manifest "$image" "$dest/android-arm64" android-arm64
  else
    echo "no Android build yet: skipped"
  fi
  echo "release: $dest"
}

cmd_update() {
  local target=${1:-latest} pinned image
  require_idle
  pinned=$(pinned_version)
  [[ $target == latest ]] && target=$(latest_stable)
  [[ $target =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "not a version: $target"
  if [[ $target == "$pinned" ]]; then
    echo "already on $target; rebuilding"
  fi

  say "pin $target (was $pinned)"
  sed -i "s/^ARG CHROMIUM_TAG=.*/ARG CHROMIUM_TAG=$target/" "$DOCKERFILE"
  image=$(image_for "$target")

  say "image $image"
  docker build -q -t "$image" "$REPO/chorium/docker"

  say "sync the checkout to $target (Windows + Android)"
  in_container "$image" bash /umbra/chorium/docker/fetch.sh win android
  check_winsdk "$image"

  say "apps (WebAssembly)"
  docker run --rm -v "$VOLUME:/work" -v "$REPO_DOCKER:/umbra:ro" \
    -e OUT=/work/wasm/out "$image" bash /umbra/chorium/docker/build_wasm.sh app browser

  say "engine: Windows"
  in_container "$image" bash /umbra/chorium/docker/build.sh win
  say "engine: Android"
  in_container "$image" bash /umbra/chorium/docker/build.sh android

  cmd_package
  test_windows "$OUT_ROOT/releases/$target/windows-x64"
  say "done: Chromium $target. Commit chorium/docker/Dockerfile."
}

case ${1:-} in
  check) cmd_check ;;
  update) shift; cmd_update "${1:-latest}" ;;
  package) cmd_package ;;
  *) sed -n '3,22p' "$0"; exit 1 ;;
esac
