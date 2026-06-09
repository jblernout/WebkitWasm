#!/usr/bin/env bash
# Export all local modifications to the pinned WebKit checkout as one
# cumulative patch in src/patches/. Run after every WebKit source fix.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/src/patches"
git -C "$ROOT/third_party/WebKit" diff > "$ROOT/src/patches/webkit-emscripten.patch"
wc -l "$ROOT/src/patches/webkit-emscripten.patch"
