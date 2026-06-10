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
│  │  Skia CPU raster (vendored) · libcurl+OpenSSL (SOCKFS→Wisp)    │  │
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
  - [x] host prereq: ruby 3.3.7 installed (required by JSC's offlineasm,
        which generates the CLoop interpreter)
  - GATE: **PASSED 2026-06-09** — no hard blocker in the port surface.
    The one novel, unproven piece (single-process embedding of
    WebCore::Page) moves to Phase 2's kill-gate.
- [ ] **Phase 1 — JSC proof-of-life**
  - Build JavaScriptCore (CLoop, JSCOnly port) to wasm; run JS in the page.
  - [x] 2026-06-09: jsc.wasm builds (45 MB with embedded ICU data) and RUNS
        JS under node: `print(6*7)` → 42, exit 0. Nine WebKit patches total
        (src/patches/webkit-emscripten.patch). Key link flags: STACK_SIZE=8MB
        (64KB default = instant empty-message StackOverflowError),
        INITIAL_MEMORY=128MB, ALLOW_MEMORY_GROWTH, embed icudt77l.dat at
        ICU's compiled-in default path.
  - [x] Browser half PASSED 2026-06-09: headless Chromium, COOP/COEP
        served, crossOriginIsolated=true, `JSC-in-tab: 42` (web/gate1.html
        + tools/gate1-browser-test.mjs).
  - GATE: **PASSED 2026-06-09** — WebKit's JS engine runs JS in a tab.
    Phase 2 (First paint: WebCore + Skia + canvas) is next.
  - First-of-its-kind: zero Bugzilla precedent for emcmake builds of the
    current tree; JSC.js (2021) bypassed WebKit's CMake with custom GN.
  - PROBE RESULT 2026-06-09: `emcmake cmake -DPORT=JSCOnly -DENABLE_JIT=OFF`
    runs deep — ALL compiler/platform checks pass under emcc 6.0.0; first
    blocker is exactly as predicted: ICU ≥70.1 (OptionsJSCOnly.cmake:117).
    The build system is not hostile to emcmake. ICU wasm build underway
    (tools/build-deps/icu.sh, ICU 77.1, static data packaging — fallback
    to archive packaging if pkgdata can't emit wasm-compatible objects).
  - GATE: JSC runs scripts in-tab. Kills the project early if the WebKit
    build system fundamentally fights Emscripten.
- [ ] **Phase 2 — First paint**
  - [x] 2026-06-09: full dependency tier built to wasm-sysroot in one run
        (tools/build-deps/webcore-deps.sh): zlib, libpng, libjpeg-turbo,
        libwebp, freetype, harfbuzz(+icu), libxml2, sqlite3 — all static,
        all -pthread.
  - [x] PORT=Emscripten skeleton: OptionsEmscripten.cmake +
        PlatformEmscripten.cmake modeled on PlayStation port (2026-06-09).
  - [x] curl tier built to wasm-sysroot (decision-003): OpenSSL 3.5.0,
        nghttp2, brotli, libpsl(+ICU), curl 8.17, fontconfig 2.15
        (etc/fonts staged in sysroot for runtime packaging).
  - [x] 2026-06-09: WebCore configure PASSES and **libWebCore.a COMPILES**
        (plus libJavaScriptCore/libWTF/libPAL/libSkia in build/webcore/lib).
        Patch ledger 640 lines; Codex-reviewed (0 critical/high, 3 findings
        fixed). WebCrypto deferred (BoringSSL-flavored upstream sources).
  - [x] 2026-06-09 ~23:45: **GATE PASSED — first paint, offscreen half.**
    embedder.wasm (87.6 MB; src/embedder/ + 12 WebKit-tree stub files)
    renders `<h1>hello</h1>` + styled div 800x600 PIXEL-EXACT under node
    (#0066CC div exactly 20000 px, antialiased DejaVu glyphs, real font
    metrics in the render tree). Built from EmptyClients.h
    pageConfigurationWithEmptyClients exactly as planned; 48 undefined
    symbols → one stub iteration (playbook memory has the recipe).
    Single-threaded (matches the lib stack: no -pthread anywhere).
    Commits a680190 (links) → da01669 (gate). Output: build/out.png.
  - [x] 2026-06-10 ~00:09: **Canvas half PASSED.** Same pixels blitted to a
    <canvas> in a COOP/COEP Chromium tab (GATE2-BROWSER: PASS, identical
    counts: exactBlue=20000 redGlyph=1541, 11 sustained rAF frames).
    Engine stays alive via emscripten_exit_with_live_runtime(); host page
    web/browser.html drives exported bib_tick()/bib_render() and blits the
    unpremul RGBA buffer via putImageData. Proof: build/gate2-canvas.png.
    Offscreen node gate unchanged and still passing.
  - GATE: "hello <h1>" pixels — PASSED offscreen 2026-06-09, in-tab
    2026-06-10.
- [x] **Phase 3 — Interactivity** (core COMPLETE 2026-06-10 ~01:35)
  - [x] Mouse plumbing: move/press/release → EventHandler; :hover restyle
    repaints via BibChromeClient dirty flag (invalidation-driven; clean
    frames skip the blit entirely).
  - [x] SCRIPT ON: JSC (CLoop) executes inside the engine page — onclick
    mutates DOM, event listeners fire. Root cause fixed:
    pageConfigurationWithEmptyClients hardcodes SandboxFlags::all()
    (SVGImage semantics) — cleared on the main frame in interactive mode.
  - [x] Keyboard + text input: RawKeyDown/Char/KeyUp → EventHandler;
    BibEditorClient (EmptyEditorClient surface, gates flipped true,
    WinCairo key-command map) inserts typed text into <input> fields.
  - [x] Wheel/trackpad scrolling: root cause was ScrollAnimator's
    scrollAnimationEnabled defaulting TRUE off the COORDINATED_GRAPHICS
    guard — animations never tick (no display-refresh driver) → guard-join
    patch routes to Settings → immediateScrollBy (synchronous).
  - [x] GATE 3 PASSED: 8/8 pixel assertions via real Playwright input
    (tools/gate3-browser-test.mjs; proof build/gate3-canvas.png). All
    three gates green (offscreen byte-identical, gate2 identical counts).
  - [ ] Deferred to later phases: images (need resource loading — Phase 4
    loader), full CSS fidelity audit, smooth scrolling (needs a rAF-driven
    scroll-animation driver), IME/composition input.
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
| ONE MUTATOR THREAD PER VM (Codex review 2026-06-09): Thread::suspend is impossible under wasm; conservative GC scanning a second registered VM thread would trap. Web Workers are safe (own VM per thread); JSC API clients sharing a VM across threads are NOT | MED | Deterministic RELEASE_ASSERT at suspend() entry with explicit message; ENABLE_SAMPLING_PROFILER stays OFF; revisit only if a shared-VM design ever appears |
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
- network stack: **curl 8.17 + OpenSSL 3.5 + nghttp2 + zlib + brotli + libpsl**
  (decision-003 — WebCore's curl backend hardwires the OpenSSL API; mbedTLS
  pin was libcurl.js-specific and is superseded)
- raster: **vendored Skia** (`Source/ThirdParty/skia`), CPU raster
- threading: `-pthread -sPROXY_TO_PTHREAD`, pre-sized pool; no Asyncify
