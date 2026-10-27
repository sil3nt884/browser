#!/usr/bin/env bash
# Copyright 2026 The Umbra Authors.
# Builds the Umbra Remote APK (run setup-sdk.sh once first) and copies it to
# <repo>/dist/UmbraRemote.apk. The network link (remote/net, C) is compiled
# with the NDK's clang for each phone CPU type first.
set -euo pipefail

ROOT="${ANDROID_BUILD_HOME:-$HOME/android-build}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export JAVA_HOME="$ROOT/jdk"
export ANDROID_HOME="$ROOT/sdk"
export PATH="$JAVA_HOME/bin:$PATH"

NDK="$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)"
if [ -z "$NDK" ]; then
  echo "no NDK in $ANDROID_HOME/ndk (run setup-sdk.sh)" >&2
  exit 1
fi
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
NET="$HERE/../net"
for pair in arm64-v8a:aarch64-linux-android26 armeabi-v7a:armv7a-linux-androideabi26 x86_64:x86_64-linux-android26; do
  abi="${pair%%:*}"
  target="${pair#*:}"
  out="$HERE/app/build/jni/$abi"
  mkdir -p "$out"
  "$CLANG/$target-clang" -O2 -fPIC -shared -std=c11 -Wall -Wextra -Werror \
    -Wl,-z,max-page-size=16384 -Wl,--gc-sections -ffunction-sections -fdata-sections -s \
    -I "$NET" "$NET/umbranet.c" "$HERE/app/src/main/c/link_jni.c" -o "$out/libumbranet.so"
done
echo "libumbranet.so built with $(basename "$NDK")"

cd "$HERE"
echo "sdk.dir=$ANDROID_HOME" > local.properties
if [ ! -f gradlew ]; then
  "$ROOT/gradle/bin/gradle" wrapper --gradle-version 8.10.2 --no-daemon
fi
chmod +x gradlew
./gradlew --no-daemon assembleRelease
mkdir -p "$HERE/../../dist"
cp app/build/outputs/apk/release/app-release.apk "$HERE/../../dist/UmbraRemote.apk"
echo "APK: $(cd "$HERE/../../dist" && pwd)/UmbraRemote.apk"
