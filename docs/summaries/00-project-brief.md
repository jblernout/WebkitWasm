# 00 — Project Brief: BrowserInBrowser (WebKit → WASM)

**Status**: Phase 0 — research complete 2026-06-09 (both reports in
docs/research/); toolchain setup underway
**Decision records**: decision-001-architecture.md ·
decision-002-toolchain-pins.md

## One-liner
Compile WebKit itself (WebCore + JavaScriptCore) to WebAssembly so a fully
modern browser engine runs inside a browser tab, renders into an
interactable `<canvas>`, and does all networking over the Wisp protocol.

## What we are NOT doing
- No x86 emulation (v86, QEMU-wasm, container2wasm) — rejected.
- No proprietary VM (CheerpX/WebVM) — rejected.
- No WebKit2 multiprocess port — single-process, WebKit1-style embedder.
- No publishing/deployment until explicitly asked. Local git only.

## Architecture
```
┌────────────────────────── host browser tab ──────────────────────────┐
│  host page (JS/TS)                                                   │
│  ┌─────────────┐   input events    ┌──────────────────────────────┐  │
│  │  <canvas>   │ ◄──────────────── │  embedder shell (our code)   │  │
│  │  (renderer) │ ──framebuffer───► │  tabs / URL bar / history    │  │
│  └─────────────┘                   └───────────────┬──────────────┘  │
│                                                    │ C API           │
│  ┌─────────────────────────────────────────────────▼──────────────┐  │
│  │  WebKit.wasm  (Emscripten, pthreads, no JIT)                   │  │
│  │  JSC (CLoop) · WebCore · WTF · FreeType/HarfBuzz · ICU ·       │  │
│  │  Skia CPU raster (vendored) · libcurl+mbedTLS (SOCKFS→Wisp)    │  │
│  └───────────────────────────────────┬────────────────────────────┘  │
│                                      │ Wisp client (mux'd streams)   │
└──────────────────────────────────────┼───────────────────────────────┘
                                  WebSocket
                                       │
                       wisp-js (dev) / epoxy-server (perf)
                                       │
                                  real internet
```
Key properties:
- TLS terminates **inside** WebKit.wasm (engine carries its own TLS stack),
  so the Wisp server is a dumb TCP/UDP relay — like a real browser behind
  a SOCKS proxy.
- Canvas is the engine's display surface; we translate DOM input events on
  the canvas into WebCore platform events.

## Phases (fail-fast gates)
- [ ] **Phase 0 — Toolchain + research** (current)
  - [x] Repo scaffold, local git, docs structure
  - [x] Research: WebKit port surface → research-01-webkit-port-surface.md
  - [x] Research: prior art / Wisp / Emscripten →
        research-02-prior-art-wisp-emscripten.md
  - [x] COOP/COEP dev server (tools/dev-server.mjs) — headers + traversal
        guard verified; smoke page at web/index.html
  - [x] wisp-js relay verified listening (npm run wisp → 127.0.0.1:5001)
  - [x] emsdk 6.0.0 installed + verified (`emcc 6.0.0` runs; 1.6 GB)
  - [x] WebKit pinned + cloned: `webkitglib/2.52` @ `aec9d2ad9` (7.5 GB;
        verified present: OptionsJSCOnly/PlayStation/Win.cmake,
        platform/network/curl, ThirdParty/skia)
  - [ ] host prereq: **ruby missing** — required by JSC's offlineasm (it
        generates the CLoop interpreter). Needs `sudo apt install -y ruby`.
  - GATE: **PASSED 2026-06-09** — no hard blocker in the port surface.
    The one novel, unproven piece (single-process embedding of
    WebCore::Page) moves to Phase 2's kill-gate.
- [ ] **Phase 1 — JSC proof-of-life**
  - Build JavaScriptCore (CLoop, JSCOnly port) to wasm; run JS in the page.
  - First-of-its-kind: zero Bugzilla precedent for emcmake builds of the
    current tree; JSC.js (2021) bypassed WebKit's CMake with custom GN.
  - GATE: JSC runs scripts in-tab. Kills the project early if the WebKit
    build system fundamentally fights Emscripten.
- [ ] **Phase 2 — First paint**
  - Minimal WebCore config; render a fixed HTML string offscreen; blit to
    canvas. No network, no events. Requires embedding WebCore::Page +
    client interfaces directly against internal headers — there is NO
    supported single-process embedding API outside Cocoa. Model on the
    PlayStation port (curl + generic RunLoop + Skia + static embedding).
  - GATE: "hello <h1>" pixels on canvas. This is the make-or-break gate.
- [ ] **Phase 3 — Interactivity**
  - Mouse/keyboard/scroll plumbing, text input, fonts, images (png/jpeg/
    webp), CSS at full fidelity.
- [ ] **Phase 4 — Networking over Wisp**
  - libcurl with custom socket layer → Wisp client → wisp server.
  - http(s) page loads end-to-end; cookies + cache in OPFS.
- [ ] **Phase 5 — Browser chrome**
  - Tabs, URL bar, history, loading indicators around the canvas.
- [ ] **Phase 6 — Performance**
  - Skia→WebGL2 GPU path, SIMD, threading tuning, maybe JSPI once Safari
    ships it. (Memory64 rejected: 10–100% measured penalty, no Safari.)

## Risk register
| Risk | Sev | Mitigation |
|---|---|---|
| Single-process embedding is NOVEL: no supported non-Cocoa API; WebKitLegacy(Win) deleted 2023-02; in-tree curl stack only runs inside WebKit2's NetworkProcess | HIGH | Embed WebCore::Page + client interfaces against internal headers, modeled on PlayStation port; Phase 2 gate kills early |
| WebKit CMake under emcmake is unproven (zero Bugzilla precedent; JSC.js bypassed it entirely with custom GN) | HIGH | Phase 1 proves it on JSCOnly, the smallest target, before WebCore; patches in src/patches/ |
| Dependency drift: emscripten-ports ICU is 68.2 (< WebKit's hard floor 70.1), harfbuzz 3.2.0 (2021) | MED | Self-build + pin all ~12 deps (lift libcurl.js tools/*.sh recipes); ICU data filter (default ~30 MB → trim) |
| No JS JIT → slow JS-heavy sites | MED | Accepted; CLoop is supported + CI-tested config (fixes through 2026-04) |
| wasm32 4 GB ceiling | MED | Minimal feature flags + trimming; Memory64 rejected (perf, no Safari) |
| Total payload size | LOW | Envelope from precedent: 30–60 MB wasm, 15–25 MB brotli initial (JSC.js ≈4 MB, CanvasKit ≈1.5 MB compressed); lazy ICU/fonts |
| Upstream churn vs pinned tree | LOW | Pin one branch (webkitglib/2.52) for entire project |

Resolved by research: ~~glib dependency~~ — JSCOnly defaults to WTF generic
RunLoop with no glib; the PlayStation-port pattern avoids glib entirely.

## Open questions — RESOLVED 2026-06-09 (details + citations in docs/research/)
1. **Base port**: JSCOnly first, then a custom `PORT=Emscripten` modeled on
   the PlayStation port (curl + generic RunLoop + Skia + static embedding),
   borrowing the Win port's curl glue. Adding a port is a clean 3-file
   pattern (`ALL_PORTS` + `Options<Port>.cmake` + `Platform<Port>.cmake`).
   Curl backend confirmed healthy (functional commits through 2026-06-09;
   Win port maintained by Microsoft + Sony). Trimmed WPE rejected —
   glib/soup/WebKit2 entanglement is all deletion work.
2. **Raster**: Skia CPU is the only sane path. cairo unmaintained in WebKit
   since 2.46 (2024-09); `USE_SKIA=OFF` deleted for GTK/WPE 2026-02; WebKit
   vendors Skia at `Source/ThirdParty/skia`; CanvasKit = official
   Emscripten-Skia precedent.
3. **Wisp bridge**: pure JS — three small patches on Emscripten's SOCKFS
   WebSocket layer + wisp-js `WispWebSocket` constructor swap. No C-level
   Wisp code needed. libcurl.js (552 KB compressed) is the existence proof;
   its mbedtls/curl/nghttp2/zlib/brotli build scripts are liftable.
   wisp-server-python rejected (still Wisp v1-only).
4. **Emscripten**: Memory64 NO (10–100% penalty, no Safari). Neither JSPI
   nor Asyncify — run engine under `-pthread -sPROXY_TO_PTHREAD` with a
   pre-sized pool so blocking is legal on workers. Cookies/cache: wasmfs
   OPFS backend.
5. **Autopsies**: webkit.js (2014) died on exactly the networking+input gap
   our phasing attacks first; JSC.js proves JSC-on-wasm (CLoop, ≈4 MB
   compressed) but bypassed WebKit's build system and died 2021. Nobody
   shipped an engine-on-wasm 2020–2026 — the lane is empty.

## Pins
- WebKit: monorepo branch **`webkitglib/2.52`** (active, commits through
  2026-06-08) — full git checkout, exact hash recorded here at clone time.
  Do NOT use webkitgtk release tarballs: GTK/WPE-filtered, likely missing
  Win/PlayStation/curl files.
- WebKit hash: `aec9d2ad958e716ab4bca4bf03007e6edac7323f` (2026-06-09)
- emsdk: **6.0.0** (2026-06-04; fall back to latest 5.x if it misbehaves)
- wisp server: **wisp-js 0.4.1** (dev); epoxy-server later for perf
- network stack: **curl 8.17 + mbedTLS + nghttp2 + zlib + brotli**
  (libcurl.js recipe)
- raster: **vendored Skia** (`Source/ThirdParty/skia`), CPU raster
- threading: `-pthread -sPROXY_TO_PTHREAD`, pre-sized pool; no Asyncify
