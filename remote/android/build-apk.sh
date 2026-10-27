#!/usr/bin/env bash
# Copyright 2026 The Umbra Authors.
# Builds the Umbra Remote APK (run setup-sdk.sh once first) and copies it to
# <repo>/dist/UmbraRemote.apk.
set -euo pipefail

ROOT="${ANDROID_BUILD_HOME:-$HOME/android-build}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export JAVA_HOME="$ROOT/jdk"
export ANDROID_HOME="$ROOT/sdk"
export PATH="$JAVA_HOME/bin:$PATH"

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
