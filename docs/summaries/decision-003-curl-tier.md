# Decision 003 — curl tier: OpenSSL (not mbedTLS/BoringSSL) + Fontconfig built for wasm

**Date**: 2026-06-09 · **Status**: accepted · **Amends**: decision-002 (network
stack pin "curl + mbedTLS")

## 1. TLS stack: OpenSSL 3.5.x (LTS)

WebCore's curl backend is not TLS-agnostic — it hardwires the OpenSSL API:
- `platform/network/curl/OpenSSLHelper.cpp` (cert chain inspection, error
  mapping) — unconditional in `platform/Curl.cmake:23`.
- `platform/Curl.cmake:61` links `OpenSSL::SSL` into WebCore directly.
- PAL uses `CryptoDigestOpenSSL` on this path.
- `OptionsEmscripten.cmake:126` therefore `find_package(OpenSSL REQUIRED)`.

mbedTLS (decision-002's pin, lifted from libcurl.js) only ever applied to the
SOCKFS→Wisp shim pattern — libcurl.js has no WebCore on top. Rejected.

BoringSSL rejected too: CMake's stock `FindOpenSSL` parses real
`opensslv.h` version macros; the Win port (donor of our curl glue) builds and
tests against real OpenSSL, so OpenSSL proper has zero API-drift risk.
**Confidence: high.**

## 2. Fontconfig: build it for wasm, do NOT patch it out

The probe queued Fontconfig behind curl (vendored
`skia/CMakeLists.txt:6` REQUIREs it on non-Win/non-PlayStation). Patching it
out looked tempting (CanvasKit ships a custom fontmgr) but WebCore itself is
fontconfig-coupled, not just Skia:
- `FontCacheSkia.cpp:69` → `SkFontMgr_New_FontConfig(FcConfigReference(nullptr), …)`
- `SkiaSystemFallbackFontCache.cpp:31` → `#include <fontconfig/fontconfig.h>`
- Skia's Unix path compiles `SkFontMgr_fontconfig.cpp` +
  `SkFontConfigInterface_direct*.cpp` (vendored CMakeLists ~line 838-846).

Patching this out = rewriting WebCore's font cache + system-fallback cache =
permanent fork of text. Building fontconfig keeps us byte-for-byte on the
GTK/WPE-tested code path. Runtime needs (fonts.conf + font files in the wasm
FS) land in Phase 2's render step, where we must bundle fonts anyway.
**Confidence: high.**

## 3. Resulting tier (tools/build-deps/curl-tier.sh), in build order

| Dep | Version | Why |
|---|---|---|
| OpenSSL | 3.5.0 (LTS) | WebCore links OpenSSL::SSL; floor: none stated |
| nghttp2 | 1.64.0 | HTTP/2; cheap now, avoids Phase 4 curl rebuild |
| brotli | 1.1.0 | content-encoding; same rationale |
| libpsl | 0.21.5 | `find_package(LibPSL 0.20.2)`; ICU runtime (sysroot has icu-uc) |
| curl | 8.17.0 | floor 7.85 (OptionsEmscripten.cmake:125); built against all above |
| fontconfig | 2.15.0 | floor 2.13 (skia/CMakeLists.txt:6); libxml2 backend (in sysroot) |

All static, `-O2 -pthread` (pthread ABI must match the engine), installed to
`third_party/wasm-sysroot`. curl protocol surface trimmed to http(s)/ws(s)/
file; IPv6 and threaded resolver off (SOCKFS DNS is synchronous-fake;
libcurl.js precedent). CA bundle path pinned to `/etc/ssl/ca-bundle.crt` —
populated in the wasm FS at Phase 4.

## Deferred
- TextureMapper/compositing: still OFF; verify at WebCore compile stage
  (decision-002 question stands).
- epoxy-server swap and CA-bundle delivery: Phase 4.
