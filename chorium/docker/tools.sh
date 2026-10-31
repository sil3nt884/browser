#!/bin/bash
# Copyright 2026 The Umbra Authors.
#
# Unpacks the tools Chorium's build needs besides Chromium into the volume,
# once (re-running skips what is there):
#
#   docker run --rm -v chorium-work:/work -v C:/me-tor/umbra:/umbra:ro \
#     chorium-build:157 bash /umbra/chorium/docker/tools.sh
#
#   /work/wasm-tools  wasi-sdk 34, wit-bindgen 0.63, wac 0.12 (build_wasm.sh),
#                     Wasmtime 50.0.1's C API for Android, Windows and Linux
#                     (tests/app), and the Material Icons font (the Android
#                     runtime's icons)
#   /work/rust        rustup's Rust with the Android target (Tor library)
#   /work/android-ndk the official NDK r27c (Chromium's copy has no clang
#                     driver for Rust's cc builds)
set -euo pipefail

T=/work/wasm-tools
mkdir -p "$T" "$T/fonts" /work/android-ndk

fetch() {  # url, file
  if [[ ! -f $2 ]]; then
    curl -fL --retry 3 -o "$2.part" "$1"
    mv "$2.part" "$2"
  fi
}

GH=https://github.com
cd "$T"
if [[ ! -d wasi-sdk-34.0-x86_64-linux ]]; then
  fetch "$GH/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/wasi-sdk-34.0-x86_64-linux.tar.gz" wasi-sdk.tgz
  tar xzf wasi-sdk.tgz && rm wasi-sdk.tgz
fi
if [[ ! -d wit-bindgen-0.63.0-x86_64-linux ]]; then
  fetch "$GH/bytecodealliance/wit-bindgen/releases/download/v0.63.0/wit-bindgen-0.63.0-x86_64-linux.tar.gz" wit-bindgen.tgz
  tar xzf wit-bindgen.tgz && rm wit-bindgen.tgz
fi
if [[ ! -x wac/wac ]]; then
  mkdir -p wac
  fetch "$GH/bytecodealliance/wac/releases/download/v0.12.0/wac-cli-x86_64-unknown-linux-musl" wac/wac
  chmod +x wac/wac
fi
for target in aarch64-android x86_64-windows x86_64-linux; do
  dir=wasmtime-v50.0.1-$target-c-api
  [[ -d $dir ]] && continue
  if [[ $target == *windows ]]; then
    fetch "$GH/bytecodealliance/wasmtime/releases/download/v50.0.1/$dir.zip" "$dir.zip"
    unzip -q "$dir.zip" && rm "$dir.zip"
  else
    fetch "$GH/bytecodealliance/wasmtime/releases/download/v50.0.1/$dir.tar.xz" "$dir.tar.xz"
    tar xJf "$dir.tar.xz" && rm "$dir.tar.xz"
  fi
done
fetch "$GH/google/material-design-icons/raw/master/font/MaterialIcons-Regular.ttf" \
  fonts/MaterialIcons-Regular.ttf

cd /work/android-ndk
if [[ ! -d android-ndk-r27c ]]; then
  fetch https://dl.google.com/android/repository/android-ndk-r27c-linux.zip ndk.zip
  unzip -q ndk.zip && rm ndk.zip
fi

export RUSTUP_HOME=/work/rust/rustup CARGO_HOME=/work/rust/cargo
if [[ ! -x $CARGO_HOME/bin/cargo ]]; then
  curl -fsSL https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal
fi
"$CARGO_HOME/bin/rustup" target add aarch64-linux-android
echo "TOOLS DONE"
