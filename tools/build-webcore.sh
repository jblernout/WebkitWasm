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
# Guard checks ALL artifacts, not just the TTF — a partial staging (TTF
# present, configs missing) must re-stage, and staging that produces an
# empty conf.d must FAIL, not print OK (Codex review).
if [ ! -f "$FSROOT/fonts/DejaVuSans.ttf" ] \
   || [ ! -f "$FSROOT/etc-fonts/fonts.conf" ] \
   || [ -z "$(ls "$FSROOT/etc-fonts/conf.d" 2>/dev/null)" ]; then
  rm -rf "$FSROOT"
  mkdir -p "$FSROOT/etc-fonts/conf.d" "$FSROOT/fonts"
  cp -f "$SYSROOT/etc/fonts/fonts.conf" "$FSROOT/etc-fonts/"
  for link in "$SYSROOT/etc/fonts/conf.d/"*.conf; do
    cp -f "$SYSROOT/share/fontconfig/conf.avail/$(basename "$link")" \
      "$FSROOT/etc-fonts/conf.d/" 2>/dev/null || true
  done
  cp -f /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$FSROOT/fonts/"
  CONFD_COUNT=$(ls "$FSROOT/etc-fonts/conf.d" | wc -l)
  if [ "$CONFD_COUNT" -lt 1 ]; then
    echo "FONT STAGING FAILED: conf.d is empty (sysroot fontconfig broken?)"
    exit 1
  fi
  echo "FONT STAGING: OK ($CONFD_COUNT conf.d files)"
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
else
  # Re-sync the embedder cache vars whenever any cached VALUE differs from
  # what this script would pass — a stale path must not survive in the
  # cache just because the variable exists (Codex review).
  NEED_RECONFIG=0
  for flag in "${EMBEDDER_FLAGS[@]}"; do
    entry="${flag#-D}" # NAME=VALUE
    name="${entry%%=*}"
    want="${entry#*=}"
    have=$(rg -m1 "^${name}:" "$BUILD/CMakeCache.txt" 2>/dev/null | sed 's/^[^=]*=//')
    if [ "$have" != "$want" ]; then
      NEED_RECONFIG=1
      break
    fi
  done
  if [ "$NEED_RECONFIG" = 1 ]; then
    cmake -S "$TP/WebKit" -B "$BUILD" "${EMBEDDER_FLAGS[@]}" \
      > "$ROOT/build/webcore-reconfigure.log" 2>&1
    echo "RECONFIGURE (embedder vars): OK"
  fi
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
