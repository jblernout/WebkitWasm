#!/usr/bin/env bash
# Phase 1: build JavaScriptCore (JSCOnly port, CLoop interpreter) to wasm.
# Resumable: configure is skipped if build.ninja exists; ninja is incremental.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
SYSROOT="$TP/wasm-sysroot"
BUILD="$ROOT/build/jsc"

source "$TP/emsdk/emsdk_env.sh" > /dev/null 2>&1
cd "$ROOT"

if [ ! -f "$BUILD/build.ninja" ]; then
  emcmake cmake -S "$TP/WebKit" -B "$BUILD" -GNinja \
    -DPORT=JSCOnly \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_JIT=OFF \
    -DENABLE_C_LOOP=ON \
    -DENABLE_STATIC_JSC=ON \
    -DUSE_GLIB=OFF \
    -DUSE_SYSTEM_MALLOC=ON \
    -DICU_ROOT="$SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
    -DJSC_EMBED_ICU_DATA_FILE="$SYSROOT/share/icu/77.1/icudt77l.dat" \
    > "$ROOT/build/jsc-configure.log" 2>&1
  echo "CONFIGURE: OK"
fi

# -k 50: keep building past failures so each run surfaces a BATCH of
# errors to fix, not just the first one
ninja -C "$BUILD" -k 50 jsc > "$ROOT/build/jsc-ninja.log" 2>&1 || {
  echo "NINJA FAILED — unique errors:"
  rg -n 'error:' "$ROOT/build/jsc-ninja.log" | sort -t: -k4 -u | head -25
  exit 1
}
echo "NINJA: OK"
ls -la "$BUILD/bin/" 2>/dev/null || true
