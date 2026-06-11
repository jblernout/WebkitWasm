# Decision 005 — Skia GPU backend (WebGL2/Ganesh) scope + acceleration ladder

Date: 2026-06-11. Status: **SCOPED, NOT STARTED** (user direction:
blit-shift + SIMD ship first; this is the prep for the GPU phase).
Motivation: heavy visual+JS sites drop to a few fps. Measured anatomy:
full-viewport Skia CPU paint **32.09ms** (old.reddit, >1 frame budget
before any JS), CLoop JS 10–100× slower than JIT (unfixable — no JIT in
wasm), sync image decode on main. GPU paint attacks the largest fixable
term and deletes the readPixels/putImageData blit entirely.

## Verified findings (2026-06-11, against the pinned tree + build cache)

1. **The GPU backend is ALREADY COMPILED into our libSkia.a.** The
   vendored Skia CMakeLists defines `SK_GL` + `SK_GANESH`
   (ThirdParty/skia/CMakeLists.txt:1057-1058) and compiles the full
   `src/gpu/ganesh/**` + `src/gpu/ganesh/gl/**` source set
   unconditionally — including `GrGLAssembleInterface` /
   `GrGLAssembleGLESInterfaceAutogen` (lines 420-423). Only the
   platform interface-makers (epoxy/EGL, lines 849-859) are conditional.
   We are linking a GPU-capable Skia and using none of it.
2. **No `GrGLMakeWebGLInterface` in the vendored tree** — use the
   CanvasKit pattern instead: `GrGLAssembleGLESInterface` with a getProc
   that calls `emscripten_GetProcAddress` (Emscripten's GLES2→WebGL JS
   library provides it), or vendor upstream's small webgl interface file.
3. **WebCore's Skia layer is GPU-aware out of the box.**
   `GraphicsContextSkia` takes a `RenderingMode` (we currently pass
   `Unaccelerated` in the embedder); accelerated canvas paths + GL fences
   exist (GraphicsContextSkia.cpp:326, :1142, :1302); the
   `GrDirectContext` is sourced from
   `PlatformDisplay::sharedDisplay().skiaGrContext()` — the GTK idiom.
   Integration = provide that context on this port + flip the mode.
4. **Whole engine is scalar wasm** — no `-msimd128` anywhere
   (CMakeCache CXX_FLAGS empty; deps built scalar; libjpeg-turbo
   explicitly `WITH_SIMD=0`). Skia blitters + decoders are exactly what
   wasm SIMD accelerates (CanvasKit ships SIMD): expected **1.5–3× CPU
   raster/decode** from a compile flag.
5. **Scroll reality-check**: WebKit's fast-scroll path
   (`ScrollView::scrollContents` → `hostWindow->scroll()`) only engages
   when `canBlitOnScroll()` — pages with fixed/sticky elements (GitHub's
   header, most modern sites) take the SLOW path = full-viewport
   invalidate per scroll tick regardless of any embedder blit-shift. So:
   blit-shift fixes plain/docs/old-web scrolling; **GitHub-class scroll
   needs SIMD (32→~12-20ms) and ultimately GPU (32→~2-5ms)**.

## The acceleration ladder (agreed order)

1. **Blit-shift scroll** (embedder-only, ~1 day) — implement
   `ChromeClient::scroll` as a real pixel shift + exposed-strip damage.
   Requires splitting "WebCore must repaint" (paint rect) from "host
   canvas must re-upload" (upload rect) in the dirty machinery. Helps
   plain pages only (finding 5). SHIP FIRST.
2. **`-msimd128` rebuild** (compile flag + full tree rebuild, hours) —
   broad CPU raster/decode/JS-interpreter speedup. SIMD objects link fine
   against scalar objects (no ABI change, unlike atomics) — sysroot can
   stay scalar initially; jpeg-turbo SIMD later. SHIP FIRST (same
   rebuild as #1). Gates must be re-verified (Skia SIMD blitters are
   designed to match scalar, usually bit-exact — confirm).
3. **Skia GPU (this doc's main subject)** — phases below, ~1.5–2.5 weeks.
4. **Request blocklist** (1–2 days, independent) — the only lever that
   touches the CLoop JS wall: filter analytics/ads/telemetry in
   BibLoaderStrategy (ENABLE_CONTENT_EXTENSIONS exists but is OFF and
   heavier than needed).
5. **W-B pthreads** — re-framed: parallel image decode + (with major
   extra work) tiled CPU raster. If GPU paint ships, tiled CPU raster is
   moot. Stays HOLD.

## GPU phases

- **G1 — context spike (1–2 days, throwaway allowed):** link flags
  `-sMAX_WEBGL_VERSION=2` (+`-sGL_ENABLE_GET_PROC_ADDRESS`), create a
  WebGL2 context on `#screen` via `emscripten_webgl_create_context`,
  assemble the Gr interface (finding 2), `GrDirectContexts::MakeGL`,
  `SkSurfaces::WrapBackendRenderTarget` on framebuffer 0, draw a gradient
  + text. Proves the whole stack outside WebCore.
- **G2 — engine integration (3–7 days):** port-side
  `PlatformDisplay`/`skiaGrContext` provider (resolve OQ1),
  embedder paints into the GPU surface with
  `RenderingMode::Accelerated`, present = `flushAndSubmit()` (+ implicit
  commit on event-loop return), DELETE the readPixels→HEAPU8→putImageData
  blit and the host-side ImageData path. Dirty-rect survives as clip
  (less GPU raster per frame).
- **G3 — probe/gate compatibility (1–2 days):** `bib_render(force)`
  gains a GPU readback path (surface->readPixels — slow, probes only).
  Pixel-exact gates (exactBlue=20000 etc.) WILL break under GPU AA →
  gate/node mode keeps the raster backend (runtime backend choice at
  boot; both are in the binary already per finding 1); add a
  tolerance-based GPU smoke gate.
- **G4 — validation:** gates both backends, 5-site sweep, scroll-fps
  measurement on GitHub-class pages, dirty-cost + paint-cost probes
  re-run on GPU.

## Open questions (resolve in G1)

- **OQ1**: What does `PlatformDisplay` do on PORT=Emscripten today —
  exists, stubbed, or absent? (Decides whether we implement
  `PlatformDisplayEmscripten` or bypass with a port-local accessor.)
- **OQ2**: `GLFence` under WebGL2 (accelerated-canvas path uses fences;
  WebGL2 has `fenceSync` — verify Emscripten exposes what GLFence needs,
  else guard those paths).
- **OQ3**: Skia raster-image→texture upload caching behavior under
  Ganesh for our image-heavy pages (expected: transparent + cached).
- **OQ4**: Memory accounting — GPU textures live host-side, RELIEVING the
  wasm32 4GB ceiling for backing stores; decode buffers stay in-wasm.
- **OQ5**: Interplay with guest WebGL (#32): after G2 a context exists —
  guest WebGL could render into FBOs on the SAME context and composite
  zero-copy (vs CPU readPixels). G2 likely makes #32 substantially
  cheaper; decide #32's route AFTER G1 results.

## Risk register

1. WebCore Skia paths that peek/read pixels mid-paint under Ganesh
   (filters, getImageData) — Skia handles most internally; audit during
   G2. **Medium.**
2. Emscripten GL is main-thread-bound — we are main-thread. **Low.**
3. Context loss (tab backgrounding/GPU reset) — needs a
   recreate-and-full-damage handler. **Medium, deferable.**
4. AA/precision differences breaking pixel-exact tooling — handled by G3
   dual-backend policy. **Low once G3 lands.**
5. wasm size growth from newly-referenced Ganesh objects at link.
   **Low** (already compiled; linker GC currently drops them).
