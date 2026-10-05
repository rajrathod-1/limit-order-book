#!/usr/bin/env bash
# Compiles the engine to WebAssembly for the browser demo. Needs Emscripten
# (`brew install emscripten`). The output is committed, so the static site
# deploys without a C++ toolchain; rerun this after changing include/lob.
set -euo pipefail
cd "$(dirname "$0")"

em++ -std=c++20 -O3 -DNDEBUG -I../include wasm.cpp -o engine.js \
    -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH \
    -sEXPORTED_FUNCTIONS=_reset,_limit,_market,_cancel,_depth,_fills,_live_orders,_out_ptr,_bench \
    -sEXPORTED_RUNTIME_METHODS=HEAPF64

ls -lh engine.js engine.wasm
