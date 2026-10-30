#!/usr/bin/env bash
# Builds Chorium's WebAssembly parts in the chorium-build image:
#
#   build_wasm.sh portable    chorium.wasm, the portable browser layer
#   build_wasm.sh app NAME    chorium/apps/NAME composed with chorium.wasm
#                             into NAME.wasm, ready for any runtime
#
#   docker run --rm -v chorium-work:/work -v C:/me-tor/umbra:/umbra:ro \
#     -v <out dir>:/out chorium-build:157 \
#     bash /umbra/chorium/docker/build_wasm.sh app browser
#
# Tools (unpacked once in the volume): wasi-sdk 34, wit-bindgen 0.63, wac 0.12.
set -euo pipefail

WHAT=${1:?usage: build_wasm.sh portable | app NAME}
TOOLS=/work/wasm-tools
CC=$TOOLS/wasi-sdk-34.0-x86_64-linux/bin/clang
BINDGEN=$TOOLS/wit-bindgen-0.63.0-x86_64-linux/wit-bindgen
WAC=$TOOLS/wac/wac
IFACES=/umbra/chorium/interfaces
OUT=${OUT:-/out}
WIT=/work/wasm/wit
mkdir -p "$OUT"

# browser.wit is the root package; chorium.wit is its dependency.
rm -rf "$WIT"
mkdir -p "$WIT/deps/chorium"
cp "$IFACES/browser/browser.wit" "$WIT/"
cp "$IFACES/chorium/chorium.wit" "$WIT/deps/chorium/"

# compile WORLD OUTPUT SOURCES...: one component for WORLD. wasm32-wasip2
# makes clang link a component; reactor = no main(), the caller drives it.
compile() {
  local world=$1 output=$2
  shift 2
  local gen=/work/wasm/gen/$world
  rm -rf "$gen"
  mkdir -p "$gen"
  "$BINDGEN" c --world "$world" --out-dir "$gen" "$WIT" >/dev/null
  "$CC" --target=wasm32-wasip2 -mexec-model=reactor \
    -O2 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I "$gen" "$@" "$gen/$world.c" "$gen/${world}_component_type.o" \
    -o "$output"
}

build_portable() {
  compile portable "$OUT/chorium.wasm" /umbra/chorium/portable/*.c
  ls -la "$OUT/chorium.wasm"
}

case $WHAT in
  portable)
    build_portable
    ;;
  app)
    NAME=${2:?usage: build_wasm.sh app NAME}
    build_portable
    mkdir -p /work/wasm/apps
    compile app "/work/wasm/apps/$NAME.wasm" /umbra/chorium/apps/"$NAME"/*.c
    # The app's `browser` import is filled by chorium.wasm's export; what
    # both import from the runtime stays an import of the result.
    "$WAC" plug "/work/wasm/apps/$NAME.wasm" --plug "$OUT/chorium.wasm" \
      -o "$OUT/$NAME.wasm"
    ls -la "/work/wasm/apps/$NAME.wasm" "$OUT/$NAME.wasm"
    ;;
  *)
    echo "usage: build_wasm.sh portable | app NAME" >&2
    exit 1
    ;;
esac
