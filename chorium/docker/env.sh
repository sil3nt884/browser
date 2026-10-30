# Copyright 2026 The Umbra Authors.
#
# Shared setup for fetch.sh and build.sh. depot_tools lives in the /work
# volume, because the Windows toolchain gets unpacked inside it and must
# survive from one container to the next.

if [[ ! -d /work/depot_tools ]]; then
  cp -a /home/builder/depot_tools /work/depot_tools
fi
export PATH=/work/depot_tools:$PATH
export DEPOT_TOOLS_UPDATE=0
# See python/sitecustomize.py: caps the worker count of Python generators.
export PYTHONPATH=/umbra/chorium/docker/python${PYTHONPATH:+:$PYTHONPATH}
export CHORIUM_CPU_COUNT=${CHORIUM_CPU_COUNT:-2}

if [[ -n "${WIN_TOOLCHAIN_HASH:-}" ]]; then
  export DEPOT_TOOLS_WIN_TOOLCHAIN_BASE_URL=/work/winsdk/
  if [[ -f /work/chromium/src/build/vs_toolchain.py ]]; then
    # The variable's name carries the toolchain hash Chromium asks for; its
    # value names our own package of the Windows SDK.
    _want=$(sed -n "s/^TOOLCHAIN_HASH = '\(.*\)'/\1/p" \
              /work/chromium/src/build/vs_toolchain.py)
    export "GYP_MSVS_HASH_${_want}=${WIN_TOOLCHAIN_HASH}"
  fi
fi
