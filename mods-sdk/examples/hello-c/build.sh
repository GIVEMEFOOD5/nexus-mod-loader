#!/bin/sh
# Builds mod.wasm with clang. Needs clang and lld (wasm-ld); no C library required.
set -e
cd "$(dirname "$0")"
SDK="${ES_MOD_SDK:-../../c}"
clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--strip-all -I "$SDK" -o mod.wasm hello.c
echo "Built $(pwd)/mod.wasm"
