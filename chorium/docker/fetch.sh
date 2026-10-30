#!/bin/bash
# Copyright 2026 The Umbra Authors.
#
# Fetches Chromium at $CHROMIUM_TAG into /work/chromium without history, for
# the target OSes given (default: win). Runs inside the chorium-build image:
#
#   docker run --rm -v chorium-work:/work \
#     -e WIN_TOOLCHAIN_HASH=<zip name without .zip> \
#     chorium-build:157 bash /umbra/chorium/docker/fetch.sh win
#
# The packaged Windows SDK (<hash>.zip, see chorium/README.md) must already be
# in /work/winsdk. Re-running is safe: finished steps are skipped or cheap.
set -euo pipefail

TARGETS=("${@:-win}")
WORK=/work/chromium
JOBS=${JOBS:-8}

if [[ " ${TARGETS[*]} " == *" win "* ]]; then
  : "${WIN_TOOLCHAIN_HASH:?set WIN_TOOLCHAIN_HASH to the SDK zip name}"
fi
source /umbra/chorium/docker/env.sh

mkdir -p "$WORK"
cd "$WORK"
os_list=$(printf "'%s', " "${TARGETS[@]}")
cat > .gclient <<EOF
solutions = [{
  "name": "src",
  "url": "https://chromium.googlesource.com/chromium/src.git",
  "managed": False,
  "custom_deps": {},
  "custom_vars": {"checkout_pgo_profiles": False},
}]
target_os = [${os_list%, }]
EOF

# Only the pinned tag, depth 1. `fetch chromium` would first sync main and
# all of its dependencies, then we would sync again to the tag.
if [[ ! -d src/.git ]]; then
  git init -q src
  git -C src remote add origin https://chromium.googlesource.com/chromium/src.git
fi
tag="refs/tags/$CHROMIUM_TAG"
git -C src fetch --depth=1 --no-tags origin "+$tag:$tag"
sha=$(git -C src rev-parse "$tag^{commit}")
git -C src checkout -q "$sha"

if [[ -n "${WIN_TOOLCHAIN_HASH:-}" ]]; then
  # The variable's name carries the toolchain hash Chromium's
  # build/vs_toolchain.py asks for; its value names our own package.
  want=$(sed -n "s/^TOOLCHAIN_HASH = '\(.*\)'/\1/p" src/build/vs_toolchain.py)
  export "GYP_MSVS_HASH_${want}=${WIN_TOOLCHAIN_HASH}"
  echo "Windows toolchain: GYP_MSVS_HASH_${want}=${WIN_TOOLCHAIN_HASH}"
fi

gclient sync -D --no-history --nohooks --jobs "$JOBS" --revision "src@$sha"
gclient runhooks
echo "FETCH DONE $(date -u +%FT%TZ) $(du -sh "$WORK" | cut -f1)"
