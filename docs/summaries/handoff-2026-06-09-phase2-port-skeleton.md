# Handoff — Phase 2: write the PORT=Emscripten skeleton (the ONE active handoff)

**State**: Phase 1 fully closed (gates passed, Codex-reviewed, `86045c4`).
Full WebCore dep tier in `third_party/wasm-sysroot` (`4f9cdf6`). Port file
map researched (`db77fd0`, docs/research/research-03-port-skeleton-map.md).

## Next concrete task
Create the new port and run the first WebCore configure probe. Files
(per research-03, with line refs inside):
1. `Source/cmake/WebKitCommon.cmake` — add `Emscripten` to ALL_PORTS (~line 42).
2. `Source/cmake/OptionsEmscripten.cmake` — START FROM OptionsJSCOnly.cmake
   (proven for us) and add WebCore: `USE_SKIA=ON` (vendored
   Source/ThirdParty/skia), `USE_CURL=ON`, `USE_GENERIC_EVENT_LOOP`,
   USE_SYSTEM_MALLOC=ON, ENABLE_JIT=OFF/ENABLE_C_LOOP=ON, everything else
   (video, webgl, gamepad, speech, web audio…) OFF. No WebKit2, no tools.
3. `Source/WTF/wtf/PlatformEmscripten.cmake` — crib PlatformPlayStation
   (RunLoopGeneric.cpp + generic WorkQueue).
4. `Source/WebCore/PlatformEmscripten.cmake` — `include(platform/Skia.cmake)`,
   `include(platform/Curl.cmake)`, FreeType/HarfBuzz wiring; crib
   PlatformPlayStation.cmake structure.
5. `Source/WebCore/PAL/pal/PlatformEmscripten.cmake` + likely
   JavaScriptCore/bmalloc PlatformEmscripten.cmake stubs.

Then: `emcmake cmake -DPORT=Emscripten` probe (extend tools/build-jsc.sh
pattern → tools/build-webcore.sh), iterate on the collision surface exactly
like Phase 1 (batch errors, ninja -k 50, export patches after every fix).

## STATUS UPDATE 2026-06-09 (skeleton DONE — agent run, committed)
PORT=Emscripten exists and configures: ALL_PORTS edit +
OptionsEmscripten.cmake + Platform files for WTF/JSC/WebCore/PAL +
tools/build-webcore.sh. Configure resolves ALL nine sysroot deps and stops
exactly at `Could NOT find CURL` (expected). Patch ledger now 472 lines /
14 diffs (exporter now uses --intent-to-add so new files are captured).
Probe-confirmed queue behind curl: OpenSSL → LibPSL → Fontconfig (vendored
skia/CMakeLists.txt:6 REQUIREs it on non-Win/non-PlayStation).

## DECISIONS NEEDED NEXT SESSION (before building the curl tier)
1. **TLS stack: OpenSSL, not mbedTLS** (amends decision-002!): WebCore's
   curl backend hardwires the OpenSSL API (OpenSSLHelper.cpp,
   CryptoDigestOpenSSL, links OpenSSL::SSL + LibPSL). The libcurl.js
   mbedTLS recipe applies to the SOCKFS/Wisp shim pattern, NOT the TLS
   choice. Build OpenSSL (or BoringSSL) for wasm + curl against it +
   libpsl. High confidence per agent's Codex-checked probe.
2. **Fontconfig**: build into sysroot vs patch vendored Skia's fontmgr
   selection — undecided.
3. **TextureMapper/compositing**: currently OFF; medium confidence
   Skia-CPU-raster-only compiles without it — verify at compile stage.

## Next concrete task (original spec below, partially superseded)

## Standing constraints (do not violate)
- One mutator thread per VM (Thread::suspend traps under wasm).
- ENABLE_SAMPLING_PROFILER stays OFF.
- Every WebKit edit → tools/export-webkit-patches.sh.
- Background Bash: always `cd` to project root by absolute path first.
- Memory playbook: ~/.claude/.../memory/webkit-wasm-porting-playbook.md.
