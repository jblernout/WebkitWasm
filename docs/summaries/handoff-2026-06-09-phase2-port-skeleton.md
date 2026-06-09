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

## Known gaps to expect at the probe
- **curl not built yet** — configure will fail at find_package(CURL).
  Build curl 8.x + mbedTLS + nghttp2 + brotli into the sysroot (lift
  ading2210/libcurl.js tools/*.sh recipes; see research-02). WebCore needs
  curl headers to COMPILE even though networking is Phase 4.
- **Skia**: vendored at Source/ThirdParty/skia — check how Win/WPE build it
  (research-03 §6) and whether its CMake builds standalone under emcmake.
- Scout flagged "graphics backend bridge" as the hard part — partially
  overstated: our Phase 2 plan is Skia CPU raster into a plain memory
  buffer (no device needed), embedder copies pixels to canvas
  (putImageData/ImageBitmap). The WebGL fast path is Phase 6, not now.

## Standing constraints (do not violate)
- One mutator thread per VM (Thread::suspend traps under wasm).
- ENABLE_SAMPLING_PROFILER stays OFF.
- Every WebKit edit → tools/export-webkit-patches.sh.
- Background Bash: always `cd` to project root by absolute path first.
- Memory playbook: ~/.claude/.../memory/webkit-wasm-porting-playbook.md.
