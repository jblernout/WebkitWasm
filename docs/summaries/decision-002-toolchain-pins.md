# Decision 002 — Toolchain pins & component choices

**Date**: 2026-06-09 · **Status**: ACCEPTED (synthesis of research-01 + research-02)

## Decisions
| Component | Choice | Confidence | Why |
|---|---|---|---|
| Base port strategy | JSCOnly first → custom `PORT=Emscripten` modeled on PlayStation port | high | PlayStation = curl + WTF generic RunLoop + Skia + static embedding, no glib; trimmed WPE rejected (glib/soup/WebKit2 deletion work) |
| WebKit pin | monorepo branch `webkitglib/2.52`, full git checkout | medium-high | Active (commits 2026-06-08); tarballs are GTK-filtered and likely missing Win/PlayStation/curl files |
| Raster | Vendored Skia (`Source/ThirdParty/skia`), CPU mode | high | cairo unmaintained since 2.46, `USE_SKIA=OFF` deleted 2026-02; CanvasKit = official Emscripten-Skia precedent |
| JS execution | JSC CLoop (`ENABLE_JIT=OFF`) | high | Auto-enables on unknown CPUs; CI-tested, fixes through 2026-04 |
| TLS/HTTP stack | curl 8.17 + mbedTLS + nghttp2 + zlib + brotli | high | Exact libcurl.js recipe; build scripts liftable from ading2210/libcurl.js `tools/*.sh` |
| Wisp bridge | Pure-JS shim: Emscripten SOCKFS + wisp-js `WispWebSocket` swap | high | libcurl.js proves it — no C-level Wisp code exists or is needed |
| Wisp server | wisp-js 0.4.1 (dev); epoxy-server later (perf) | medium-high | wisp-server-python REJECTED: Wisp v1-only |
| emsdk | 6.0.0 (2026-06-04); fallback latest 5.x | medium | Fresh release; fallback path defined |
| Memory model | wasm32. Memory64 REJECTED | high | 10–100% measured penalty; no Safari |
| Blocking model | `-pthread -sPROXY_TO_PTHREAD`, pre-sized pool. No Asyncify; JSPI later | medium-high | Asyncify code-size blowup is a non-starter at our binary size; JSPI lacks Safari |
| Persistence | wasmfs OPFS backend (cookies/cache) | medium | Recommended modern Emscripten FS path |

## Known unknowns carried forward
- WebKit CMake has never been driven by emcmake (zero Bugzilla precedent).
  Phase 1 (JSCOnly) is the deliberate first collision with reality.
- The in-tree curl stack only runs inside WebKit2's NetworkProcess; running
  it in-process is novel. Phase 2/4 gates own this.
- emscripten-ports versions are useless to us (ICU 68.2 < WebKit floor 70.1)
  → all ~12 deps self-built and pinned.

## Sources
- docs/research/research-01-webkit-port-surface.md
- docs/research/research-02-prior-art-wisp-emscripten.md
