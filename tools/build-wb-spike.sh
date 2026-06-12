#!/usr/bin/env bash
# W-B0 spike build — standalone, does NOT touch build/webcore.
# Output: build/wb-spike/wb-spike.js (+ .wasm), served via
#   PORT=8090 node tools/dev-server.mjs web --mount /wbspike=build/wb-spike
set -euo pipefail
cd "$(dirname "$0")/.."
source third_party/emsdk/emsdk_env.sh > /dev/null 2>&1
mkdir -p build/wb-spike

# Flags mirror the W-B1 plan (analysis-wb-engine-off-main-thread.md):
# PROXY_TO_PTHREAD moves main() to a pthread; growth+shared memory is Q2;
# --profiling-funcs keeps wasm names for the Q5 symbolization check.
# MAXIMUM_MEMORY=4GB + POOL_SIZE=4 mirror the ENGINE's link flags
# (embedder.cmake / W-B1 plan) — the 4GB growable-shared RESERVATION at
# instantiation is part of what Q2 must prove (Codex).
emcc src/spike/wb-spike.c -o build/wb-spike/wb-spike.js \
  -O1 -Wall --profiling-funcs \
  -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=4 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=4GB \
  -sENVIRONMENT=web,worker \
  -sASSERTIONS=1 \
  --pre-js src/spike/wb-spike-pre.js

echo "wb-spike built: build/wb-spike/wb-spike.js"
