#!/bin/bash
# Copyright 2026 The Umbra Authors.
#
# Builds and runs the app tests (app_tests.c) in the chorium-build image:
#
#   docker run --rm -v chorium-work:/work -v <repo>:/umbra:ro \
#     chorium-build:157 bash /umbra/chorium/tests/app/run.sh [filter]
#
# Builds browser.wasm first (build_wasm.sh), so a run checks the sources as
# they are. Needs docker/tools.sh's Wasmtime C API for Linux.
set -euo pipefail

WASMTIME=/work/wasm-tools/wasmtime-v50.0.1-x86_64-linux-c-api
OUT=/work/tests/app
mkdir -p "$OUT"

OUT=/work/wasm/out bash /umbra/chorium/docker/build_wasm.sh app browser \
  > "$OUT/build_wasm.log" 2>&1 || { cat "$OUT/build_wasm.log"; exit 1; }

R=/umbra/chorium/runtime/common
T=/umbra/chorium/tests/app
cc -std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wno-unused-parameter \
  -Wno-unknown-pragmas \
  -I "$R" -I /umbra/chorium/interfaces/native -I "$WASMTIME/include" \
  "$T/app_tests.c" "$T/fakes.c" \
  "$R/contract.c" "$R/events.c" "$R/vals.c" "$R/json.c" "$R/remote.c" \
  "$WASMTIME/lib/libwasmtime.a" -lpthread -ldl -lm \
  -o "$OUT/app_tests"

"$OUT/app_tests" /work/wasm/out/browser.wasm "$@"
