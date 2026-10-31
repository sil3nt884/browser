#!/bin/bash
# Copyright 2026 The Umbra Authors.
#
# Puts together the Windows runtime folder from a finished `build.sh win`:
# Chorium.exe, chorium.dll and what it loads, Wasmtime, the Tor library and
# the browser app (build_wasm.sh app browser).
#
#   docker run --rm -v chorium-work:/work -v C:/me-tor/umbra:/umbra:ro \
#     -v C:/me-tor/chorium-out:/out chorium-build:157 \
#     bash /umbra/chorium/docker/package_win.sh
#   -> /out/Chorium-Windows (run Chorium.exe there; tests/e2e --dist)
set -euo pipefail

OUT=/work/chromium/src/out/chorium-win
PACKAGE_NAME=${PACKAGE_NAME:-Chorium-Windows}
[[ $PACKAGE_NAME =~ ^Chorium-Windows(-[A-Za-z0-9._-]+)?$ ]] || {
  echo "Invalid Windows package name: $PACKAGE_NAME" >&2
  exit 1
}
DEST=/out/$PACKAGE_NAME
rm -rf "$DEST"
mkdir -p "$DEST/apps"
for f in Chorium.exe chorium.dll chorium.pak icudtl.dat snapshot_blob.bin \
         d3dcompiler_47.dll dxcompiler.dll libEGL.dll libGLESv2.dll \
         vk_swiftshader.dll vk_swiftshader_icd.json vulkan-1.dll; do
  cp "$OUT/$f" "$DEST/"
done
cp /work/wasm-tools/wasmtime-v50.0.1-x86_64-windows-c-api/lib/wasmtime.dll \
  "$DEST/"
cp /umbra/prebuilt/win/x64/umbra_tor.dll "$DEST/"
cp /work/wasm/out/browser.wasm "$DEST/apps/"
ls -la "$DEST" "$DEST/apps"
