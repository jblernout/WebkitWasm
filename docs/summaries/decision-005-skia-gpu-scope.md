# Decision 005 — Skia GPU backend (WebGL2/Ganesh) scope + acceleration ladder

Date: 2026-06-11. Status: **G1 PASSED 2026-06-11 — see "G1 results" below; G2 next.**
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

## G1 results (2026-06-11) — PASS, full stack proven

Spike: `src/spike/gpu-spike.cpp` → CMake target `BibGpuSpike`
(EXCLUDE_FROM_ALL, bottom of src/embedder/embedder.cmake, build with
`ninja -C build/webcore BibGpuSpike`), host page `web/gpu-spike.html`,
probe `build/gpu-spike-probe.mjs` (**must run BIB_CHANNEL=chromium**, see
finding 4). Verdict: gradient + AA circle + raster→texture image upload +
AA stroked path all render through Ganesh→GLES3→WebGL2, verified three
ways (Skia surface->readPixels, raw glReadPixels on FBO 0, host-side JS
readPixels + screenshot).

**Measured**: frame0 ≈ 85–115ms (one-time shader compiles); steady-state
full-canvas draw + FlushAndSubmit ≈ **0.9–1.3ms/frame** (vs 32.6ms CPU
full-viewport paint — different workload, but the order-of-magnitude
signal is unambiguous). Stencil=8 honored on FBO 0; GL_SAMPLES=0 with
antialias:false; wrap at sampleCnt 1 works.

**Integration requirements discovered (G2 must carry all three):**

1. **GetString shim.** Emscripten reports GL_VERSION
   `"OpenGL ES 3.0 (WebGL 2.0 (...))"`; Skia's parser
   (GrGLUtil.cpp:95) matches the `"(WebGL %d.%d"` tail and takes the
   WEBGL number → context capped at ES 2.0 → RGBA8 loses renderability
   (GrGLCaps.cpp:1524) → WrapBackendRenderTarget fails. Fix: getProc
   returns a wrapper that truncates GL_VERSION at the first '(' so the
   plain GLES branch parses 3.0. (The WebGL parser branch serves Skia's
   WebGL-standard builds, which SK_ASSUME_GL_ES=1 compiles out of our
   archive.) GLSL-version parsing is safe (ES branch matches first).
2. **glGetInternalformativ shim.** Emscripten's symbol resolves but is a
   no-op stub → GrGLCaps's per-format MSAA sample-count query reads
   nothing → fColorSampleCounts stays EMPTY → isFormatRenderable() false
   at ANY sample count for EVERY format. Fix: getProc-level
   implementation over WebGL2 getInternalformatParameter(RENDERBUFFER,
   fmt, SAMPLES); NUM_SAMPLE_COUNTS = returned array length (WebGL2
   dropped that pname).
3. **`-sFULL_ES3=1` link flag.** At parsed-ES3.0 the interface validation
   requires glMapBufferRange/glUnmapBuffer/glFlushMappedBufferRange;
   Emscripten only ships them (JS shadow-buffer emulation) under
   FULL_ES3. Engine link gains this flag in G2. If map-based transfers
   prove slow, GrContextOptions::fBufferMapThreshold can bias Skia
   toward BufferSubData — measure first.

Both shims are getProc-level — the libcurl.js pattern: pure
embedder/port code, NO Skia source patch, pinned tree stays clean.
G2 decision: keep them as shims next to the skiaGrContext provider
(preferred) rather than patching GrGLUtil.cpp.

4. **Tooling**: playwright's bundled headless-shell LOSES the WebGL
   context at the first composite (webglcontextlost at frame 2, empty
   status). Full Chromium (BIB_CHANNEL=chromium) is solid. All GPU
   gates/probes must set it — and headless-shell is a free regression
   rig for the G2 context-loss recreate handler (risk #3).
5. **Size**: spike wasm = 3.99MB (core Skia + Ganesh + runtime). The
   engine already links core Skia, so expected embedder.wasm growth in
   G2 is the Ganesh-only slice — < 4MB upper bound (risk #5 quantified).

OQ1 RESOLVED: `PlatformDisplay::sharedDisplay()` on PORT=Emscripten is a
deliberate `RELEASE_ASSERT_NOT_REACHED()` stub (see
src/patches/webkit-emscripten.patch, GL stubs file — its header comment
already says the Skia GPU phase replaces it). G2 = implement the real
provider there + the GLContext/GLFence plumbing.
OQ2 RESOLVED: fence=yes — glFenceSync/glClientWaitSync/glDeleteSync all
resolve under WebGL2. GLFence is viable in G2.
OQ3 PARTIALLY RESOLVED: raster→texture upload + sampled draw works
transparently (the spike's image block). Cache behavior under real page
load still to observe in G2.

## G2 design (recon complete 2026-06-11, task #45 — verified against pinned tree + archive)

**Upstream's own injection point does the heavy lifting.** Verified facts:

- `PlatformDisplaySkia.cpp` IS already compiled into libWebCore.a (it
  builds against Emscripten's EGL headers). Its lazy `SkiaGLContext`
  machinery (offscreen GLContext + `GrDirectContexts::MakeGL(skiaGLInterface())`)
  is gated `#if PLATFORM(GTK) || PLATFORM(WPE) || PLAYSTATION…` — our port
  falls into `return nullptr`.
- Upstream `PlatformDisplay.cpp` (NOT compiled today) has exactly the
  pattern we need: `PlatformDisplay::setSharedDisplay(unique_ptr&&)` +
  `sharedDisplay()` that asserts one was set. `Type::Surfaceless` is an
  unconditional enum value our subclass can return.
- `GLContext` (egl/GLContext.h) is concrete/final with a PUBLIC ctor
  `GLContext(GLDisplay&, EGLContext, EGLSurface, EGLConfig)` where the EGL
  types are plain `void*` typedefs. Its .cpp is EXCLUDED from our build —
  our GLStubs file already owns its member definitions (dtor +
  makeContextCurrent=false stub today). Members: ThreadSafeWeakPtr
  m_display, m_version, 3 void* handles. Once we CONSTRUCT one, the vtable
  is referenced → must also define makeCurrentImpl/unmakeCurrentImpl/
  glVersion (+ swapBuffers etc. if link demands).
- Both accelerated call sites in GraphicsContextSkia (:326 texture-backed
  draw, :1142 createAcceleratedSurface) guard with
  `skiaGLContext() && makeContextCurrent()` then RELEASE_ASSERT
  skiaGrContext() — null skiaGLContext = graceful CPU fallback.
- ImageBufferSkiaAcceleratedBackend + SkiaReplayCanvas are NOT compiled →
  ImageBuffers stay CPU; decoded raster images upload as textures
  transparently when drawn (OQ3 ✓).
- No WTF platform macro exists for the port — shared-file conditionals use
  `defined(__EMSCRIPTEN__)` (CurlContext patch precedent).

**Patch plan (WebKit tree, export via tools/export-webkit-patches.sh):**

1. Compile upstream `PlatformDisplay.cpp` + `egl/GLDisplay.cpp` on the
   port (PlatformEmscripten.cmake) — gives setSharedDisplay machinery +
   GLDisplay over Emscripten's real EGL (eglGetDisplay/eglInitialize work;
   library_egl.js provides the symbols). Drop the now-duplicate
   sharedDisplay stub from GLStubsEmscripten.cpp.
2. NEW port file `platform/graphics/emscripten/PlatformDisplayEmscripten.cpp`:
   - PlatformDisplayEmscripten subclass (type() = Surfaceless).
   - Port init called from embedder boot: emscripten_webgl_create_context
     on the engine canvas (G1 attrs incl. stencil), make current,
     setSharedDisplay(...).
   - The G1 shims (GetString truncation + glGetInternalformativ over
     getInternalformatParameter via EM_JS) + the assembled-GLES interface
     factory live HERE (spike code moves nearly verbatim).
   - GLContext member definitions for the port: ctor wraps THE canvas
     context (single-context world); makeContextCurrent →
     emscripten_webgl_make_context_current; glVersion → 300;
     createOffscreen returns the facade. (GLContext.cpp stays excluded.)
3. `PlatformDisplaySkia.cpp` hunks: (a) `skiaGLInterface()` gains
   `#if defined(__EMSCRIPTEN__)` branch → port's shimmed interface
   factory; (b) the skiaGLContext() lazy-init gate adds
   `|| defined(__EMSCRIPTEN__)`.

**Embedder plan (no patch needed):**

- BIB_GPU=1 (Module.ENV, opt-in first) boot path: port init →
  skiaGLContext() to force SkiaGLContext creation → grab skiaGrContext.
- Backing store: TEXTURE-backed SkSurface (SkSurfaces::RenderTarget) — NOT
  FBO 0 directly and NOT preserveDrawingBuffer (Chrome perf cost): paint
  dirty rects with clips into the texture surface
  (RenderingMode::Accelerated), present = backing->draw(fbo0Canvas) +
  FlushAndSubmit (GPU-GPU quad, ~free; FBO0 wrap once, G1 code).
- bib_render GPU mode: returns null pixels; host page skips the
  putImageData loop (mode flag). g_blitPixels mirror not maintained.
- Scroll: GPU mode maps g_scrollBlit → addDamage(clip) (GPU repaint of the
  scroll region ≈1–3ms; the CPU memmove trick is moot).
- BibEmbedder link flags += `-sMAX_WEBGL_VERSION=2 -sFULL_ES3=1`.
- CPU path stays bit-for-bit identical when BIB_GPU unset (gates/node).

Milestone M1 = gate page paints through Ganesh under BIB_GPU=1 while
raster gate2 stays green. Then: paint-cost/scroll-cost/sweep on GPU (M2),
backend-choice polish + GPU smoke gate (G3), full validation (G4).

## Open questions (as originally scoped; OQ1/OQ2 resolved, OQ3 partial — see G1 results)

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
