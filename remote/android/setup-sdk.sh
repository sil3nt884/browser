#!/usr/bin/env bash
# Copyright 2026 The Umbra Authors.
# Installs what the Umbra Remote APK needs to build, without sudo, into
# $ANDROID_BUILD_HOME (default ~/android-build): a JDK 21, the Android SDK
# (platform 35, build-tools 35) and Gradle. Running it again skips what's
# already there. Then:  ./build-apk.sh
#
# Note: this accepts the Android SDK License Agreement.
set -euo pipefail

ROOT="${ANDROID_BUILD_HOME:-$HOME/android-build}"
CLT_ZIP="commandlinetools-linux-11076708_latest.zip"
GRADLE_VERSION="8.10.2"
mkdir -p "$ROOT"
cd "$ROOT"

unzip_py() { # unzip without the unzip tool (not installed by default)
  python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"
}

if [ ! -x jdk/bin/java ]; then
  echo "== JDK 21 (Eclipse Temurin)"
  curl -fL --retry 3 -o jdk.tar.gz \
    "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse"
  mkdir -p jdk && tar -xzf jdk.tar.gz -C jdk --strip-components=1 && rm jdk.tar.gz
fi
export JAVA_HOME="$ROOT/jdk"
export PATH="$JAVA_HOME/bin:$PATH"

if [ ! -x sdk/cmdline-tools/latest/bin/sdkmanager ]; then
  echo "== Android command-line tools"
  curl -fL --retry 3 -o clt.zip "https://dl.google.com/android/repository/$CLT_ZIP"
  rm -rf clt && unzip_py clt.zip clt && rm clt.zip
  mkdir -p sdk/cmdline-tools && rm -rf sdk/cmdline-tools/latest
  mv clt/cmdline-tools sdk/cmdline-tools/latest && rm -rf clt
  chmod +x sdk/cmdline-tools/latest/bin/*
fi
SDKM="sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root=$ROOT/sdk"

echo "== Android SDK packages (accepting the SDK license)"
yes | $SDKM --licenses > /dev/null || true
$SDKM "platforms;android-35" "build-tools;35.0.0"

if [ ! -x gradle/bin/gradle ]; then
  echo "== Gradle $GRADLE_VERSION"
  curl -fL --retry 3 -o gradle.zip "https://services.gradle.org/distributions/gradle-$GRADLE_VERSION-bin.zip"
  rm -rf "gradle-$GRADLE_VERSION" && unzip_py gradle.zip . && rm gradle.zip
  rm -rf gradle && mv "gradle-$GRADLE_VERSION" gradle && chmod +x gradle/bin/gradle
fi

echo "== done: $ROOT"
