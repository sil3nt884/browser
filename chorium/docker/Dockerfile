# Copyright 2026 The Umbra Authors.
#
# Build environment for Chorium (Chromium's engine as a library, Tor inside).
# One image builds every target: Windows is cross-compiled with a packaged
# Windows SDK (see chorium/README.md), Android is built natively.
#
#   docker build -t chorium-build:157 chorium/docker
#
# The Chromium checkout and build output live in a Docker volume mounted at
# /work, never in a folder shared from Windows (that is many times slower).

FROM ubuntu:24.04

ARG CHROMIUM_TAG=157.0.8103.15
ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    TZ=Etc/UTC

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl git lsb-release sudo file xz-utils zip unzip bzip2 \
      python3 python-is-python3 tzdata procps time \
 && rm -rf /var/lib/apt/lists/*

# Chromium's own dependency list, taken from the exact version we build.
# --no-backwards-compatible keeps out the legacy list (Microsoft core fonts
# and their licence prompt, snapcraft, NaCl's i386 libraries).
RUN mkdir /tmp/ibd \
 && for f in install-build-deps.sh install-build-deps.py; do \
      curl -fsSL "https://chromium.googlesource.com/chromium/src/+/refs/tags/${CHROMIUM_TAG}/build/${f}?format=TEXT" \
        | base64 -d > /tmp/ibd/$f; \
    done \
 && chmod +x /tmp/ibd/* \
 && apt-get update \
 && /tmp/ibd/install-build-deps.sh --no-prompt --no-syms --no-arm \
      --no-chromeos-fonts --no-backwards-compatible --no-nacl \
 && rm -rf /tmp/ibd /var/lib/apt/lists/*

# Unprivileged build user; sudo only for the Android deps added later.
RUN userdel -r ubuntu 2>/dev/null || true \
 && useradd -m -u 1000 -s /bin/bash builder \
 && echo "builder ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/builder \
 && mkdir /work && chown builder:builder /work

USER builder
RUN git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git /home/builder/depot_tools \
 && git config --global user.name builder \
 && git config --global user.email builder@localhost \
 && git config --global core.autocrlf false

ENV PATH=/home/builder/depot_tools:$PATH \
    CHROMIUM_TAG=${CHROMIUM_TAG}
WORKDIR /work
