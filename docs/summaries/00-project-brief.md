# 00 — Project Brief: BrowserInBrowser (WebKit → WASM)

**Status**: Phase 0 (scaffold + research) — started 2026-06-09
**Decision record**: decision-001-architecture.md

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
│  │  cairo-or-Skia software raster · libcurl (custom socket layer) │  │
│  └───────────────────────────────────┬────────────────────────────┘  │
│                                      │ Wisp client (mux'd streams)   │
└──────────────────────────────────────┼───────────────────────────────┘
                                  WebSocket
                                       │
                          wisp-js / wisp-server-python
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
  - [ ] Research: WebKit port surface (can WebCore build without WebKit2?
        curl backend health? glib-free RunLoop? Skia vs cairo?)
  - [ ] Research: prior art (JSC.js, webkit.js), Wisp/libcurl.js internals,
        Emscripten 2026 state (pthreads, Memory64, JSPI, OPFS)
  - [ ] emsdk installed; COOP/COEP dev server; wisp server running locally
  - [ ] WebKit checkout pinned (exact hash recorded here)
  - GATE: research confirms no hard blocker in the port surface.
- [ ] **Phase 1 — JSC proof-of-life**
  - Build JavaScriptCore (CLoop, JSCOnly port) to wasm; run JS in the page.
  - GATE: JSC runs scripts in-tab. Kills the project early if the WebKit
    build system fundamentally fights Emscripten.
- [ ] **Phase 2 — First paint**
  - Minimal WebCore config; render a fixed HTML string offscreen; blit to
    canvas. No network, no events.
  - GATE: "hello <h1>" pixels on canvas. This is the hardest gate.
- [ ] **Phase 3 — Interactivity**
  - Mouse/keyboard/scroll plumbing, text input, fonts, images (png/jpeg/
    webp), CSS at full fidelity.
- [ ] **Phase 4 — Networking over Wisp**
  - libcurl with custom socket layer → Wisp client → wisp server.
  - http(s) page loads end-to-end; cookies + cache in OPFS.
- [ ] **Phase 5 — Browser chrome**
  - Tabs, URL bar, history, loading indicators around the canvas.
- [ ] **Phase 6 — Performance**
  - Skia→WebGL2 GPU path, SIMD, threading tuning, maybe JSPI/Memory64.

## Risk register
| Risk | Sev | Mitigation |
|---|---|---|
| WebCore won't build standalone (WebKit2 assumed everywhere) | HIGH | Research first; base on JSCOnly/PlayStation/WinCairo port patterns; Phase 2 gate kills early |
| Build-system fights (CMake + Emscripten cross) | HIGH | Pin everything; patch files in src/patches/; budget weeks not days |
| glib dependency (GTK/WPE ports) on wasm | MED | Prefer ports using WTF generic RunLoop (PlayStation/WinCairo lineage) |
| No JS JIT → slow JS-heavy sites | MED | Accepted; CLoop is supported config; revisit later |
| wasm32 4 GB ceiling | MED | Minimal feature flags; Memory64 fallback (perf cost) |
| ICU data size / total .wasm size (100 MB+?) | MED | ICU data filtering, brotli'd assets, lazy load |
| Upstream churn vs pinned tree | LOW | Pin one version for entire project |

## Open questions (research must answer)
1. Which WebKit port is the best base — JSCOnly+custom platform layer, or
   trimmed WPE/WinCairo? Is the curl network backend healthy in 2026?
2. Did WPE/GTK's Skia migration make cairo paths second-class? Which raster
   backend is least work under Emscripten?
3. Exact Wisp v2 framing + wisp-js client API; how libcurl.js wires curl's
   socket layer to Wisp; reusable pieces vs rewrite.
4. Emscripten 2026: Memory64 perf penalty, JSPI shipping status, OPFS FS
   backend maturity, pthread pool sizing under COOP/COEP.
5. Prior art autopsies: why webkit.js (2014) stalled; what JSC.js proves;
   any 2023–2026 WebCore-on-wasm attempts and their failure modes.

## Pins (fill during Phase 0)
- WebKit: _TBD (branch + hash)_
- emsdk: _TBD_
- wisp server: _TBD (wisp-js vs wisp-server-python + version)_
