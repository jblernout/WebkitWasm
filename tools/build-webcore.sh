#!/usr/bin/env bash
# Phase 2: build WebCore (PORT=Emscripten, CLoop, Skia CPU raster, curl) to wasm.
# Modeled exactly on tools/build-jsc.sh.
# Resumable: configure is skipped if build.ninja exists; ninja is incremental.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
SYSROOT="$TP/wasm-sysroot"
BUILD="$ROOT/build/webcore"

source "$TP/emsdk/emsdk_env.sh" > /dev/null 2>&1
cd "$ROOT"

# --- Embedder wasm-FS staging (fonts are load-bearing: no TTF = no text) ---
# The sysroot's etc/fonts/conf.d entries are DESTDIR-relative symlinks that
# are broken on the host, and emcc's file packager dereferences symlinks —
# stage a clean tree of REAL files for --embed-file.
FSROOT="$ROOT/build/embedder-fs"
if [ ! -f "$FSROOT/fonts/DejaVuSans.ttf" ]; then
  mkdir -p "$FSROOT/etc-fonts/conf.d" "$FSROOT/fonts"
  cp -f "$SYSROOT/etc/fonts/fonts.conf" "$FSROOT/etc-fonts/"
  for link in "$SYSROOT/etc/fonts/conf.d/"*.conf; do
    cp -f "$SYSROOT/share/fontconfig/conf.avail/$(basename "$link")" \
      "$FSROOT/etc-fonts/conf.d/" 2>/dev/null || true
  done
  cp -f /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$FSROOT/fonts/"
  echo "FONT STAGING: OK ($(ls "$FSROOT/etc-fonts/conf.d" | wc -l) conf.d files)"
fi

EMBEDDER_FLAGS=(
  -DEMSCRIPTEN_EMBEDDER_CMAKE="$ROOT/src/embedder/embedder.cmake"
  -DBIB_FONTCONFIG_ETC_DIR="$FSROOT/etc-fonts"
  -DBIB_FONTS_DIR="$FSROOT/fonts"
)

if [ ! -f "$BUILD/build.ninja" ]; then
  emcmake cmake -S "$TP/WebKit" -B "$BUILD" -GNinja \
    -DPORT=Emscripten \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_JIT=OFF \
    -DENABLE_C_LOOP=ON \
    -DENABLE_STATIC_JSC=ON \
    -DUSE_SYSTEM_MALLOC=ON \
    -DICU_ROOT="$SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
    -DJSC_EMBED_ICU_DATA_FILE="$SYSROOT/share/icu/77.1/icudt77l.dat" \
    "${EMBEDDER_FLAGS[@]}" \
    > "$ROOT/build/webcore-configure.log" 2>&1
  echo "CONFIGURE: OK"
elif ! rg -q "EMSCRIPTEN_EMBEDDER_CMAKE" "$BUILD/CMakeCache.txt"; then
  # Existing build dir from before the embedder existed: add the cache
  # entries without reconfiguring from scratch (NEVER rm -rf this dir).
  cmake -S "$TP/WebKit" -B "$BUILD" "${EMBEDDER_FLAGS[@]}" \
    > "$ROOT/build/webcore-reconfigure.log" 2>&1
  echo "RECONFIGURE (embedder vars): OK"
fi

# -k 50: keep building past failures so each run surfaces a BATCH of
# errors to fix, not just the first one
ninja -C "$BUILD" -k 50 WebCore BibEmbedder > "$ROOT/build/webcore-ninja.log" 2>&1 || {
  echo "NINJA FAILED — unique errors:"
  rg -n 'error:' "$ROOT/build/webcore-ninja.log" | sort -t: -k4 -u | head -25
  exit 1
}
echo "NINJA: OK"
# The project package.json is "type":"module"; node must treat the
# non-modularized Emscripten output as CommonJS (tools/run-embedder.cjs).
printf '{"type":"commonjs"}\n' > "$BUILD/bin/package.json"
ls -la "$BUILD/lib/" "$BUILD/bin/" 2>/dev/null || true
