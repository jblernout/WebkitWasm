# Handoff — GPU present re-architecture (zero-copy, compile-time toggleable)

**Written**: 2026-06-13 ~01:45 EDT. **Supersedes** handoff-2026-06-13-gpu-scroll-perf.md
(that doc's "scroll is weight-bound" framing was WRONG — see CORRECTION). Read this
before touching rendering/present.

---

## ✅ IMPLEMENTED & VERIFIED — 2026-06-13 ~10:45 EDT

The plan below was executed. Zero-copy bitmap present is live on the default
(BIB_PTHREAD=ON) build. Changes: `src/embedder/main.cpp` (bibPaintGPUIfDirty,
presentGPUToCanvasFBO, bibBitmapPresentReady/bibTransferCurrentFrameBitmap,
rewritten bibPushFrameIfDirty 3-branch + bib_render_readback GPU branch, boot
target + loss handlers pthread/mainthread split, worker→main hello),
`web/engine-pre.js` (present bridge), `web/browser.html` (3-mode context select +
Worker subclass + bitmap receiver + readback hello-judge), the WebKit port file
(`explicitSwapControl`/`renderViaOffscreenBackBuffer` gated behind legacy
`BIB_GPU_READBACK_PRESENT`), `embedder.cmake` (dropped `-sOFFSCREEN_FRAMEBUFFER=1`,
emits `bib-build-config.js`).

**Handshake tweak vs the GPT-5.5 design doc:** the WORKER creates the
MessageChannel and transfers `port2` to the page INSIDE the hello (worker→main
only) — so nothing custom traverses Emscripten's worker `onmessage`. There is no
main→worker `__bibPresentPort` message; that is intentional, not missing.

**Verified:** gate8 (GPU pipeline + G4 loss/restore + software-fallback) PASS;
gate2 (raster pixel-exact tol=0) PASS; headed real-GPU (Intel UHD 630) wheel→pixel
latency: Wikipedia **76–135ms**, Discord **94–388ms (median 123ms)** with 10–32
distinct visible changes per scroll — vs the broken Approach R **>4000ms / 0
distinct / frozen**. Codex-reviewed; one defensive fix applied (postMessage-throw
path closes the ImageBitmap + leaves ready=true → no leak, no wedge).

**Still open / next:** (1) BIB_PTHREAD=OFF (gpu-implicit, main-thread) path is
implemented but NOT built/verified this session — needs a separate `BIB_PTHREAD=0`
build dir + MotionMark to confirm the 109@144fps reference path. (2) User-run
MotionMark on the ON build for a steady-state number. (3) Discord still throws a
pre-existing guest `memory access out of bounds` (the #57/#70 family) — unrelated
to present; the engine survives it and keeps presenting. Original plan retained
below for reference.

---

## TL;DR (decision is made — this is an implementation handoff)

- **We regressed.** Was **MotionMark 109.87 @ 144fps** (2026-06-11, main-thread GPU,
  direct WebGL present). Now: GPU present is **flaky/near-frozen** on Discord; user
  confirms "loads homepage then frozen" in their own Brave, not just our harness.
- **Root cause: moving the engine onto a worker pthread (W-B1/W-B2) broke GPU present.**
  GPU was fast *because* it presented directly on the main thread (the SkSurface was the
  canvas; WebGL auto-presents). On the worker, every present scheme has failed:
  OffscreenCanvas placeholder commit → starved (W-B2); per-frame GPU→CPU readback →
  flaky + would never hit MotionMark anyway (Approach R, current tip `caf8f33`).
- **RASTER works reliably** (putImageData; ~130ms scroll latency). It is the safety net.
- **DECISION (user, 2026-06-13): Option 1 — zero-copy pthread present**
  (`transferToImageBitmap` → `postMessage([bitmap])` → `#screen` `bitmaprenderer`),
  **AND make it compile-time toggleable** (`BIB_PTHREAD`): people who don't want
  SharedArrayBuffer/COOP-COEP get the main-thread engine + direct GPU present instead.
- **GPT-5.5 Pro consult: GRANTED + LAUNCHED** (design the worker→main plumbing + the
  toggle matrix). Output: `/tmp/gpt55-present.log` (this session) — FOLD ITS DESIGN IN
  before implementing. Re-run with `gpt55-pro-agent --continue <url>` if the log only has
  the final diagram.

---

## CORRECTION to the previous handoff

The 2026-06-13 scroll-perf handoff concluded scroll was "weight-bound" (renderUpd/paint).
That was measured with a test that under-drove input (one wheel / 120ms → ~8/s), so
"painted ≈ 8fps" just mirrored the input rate. Under a HARD continuous scroll the engine
sustains **~45 fps painted at ~88% busy** — NOT the bottleneck. The real problem is
**present/delivery**, isolated to GPU. Do not chase renderUpd/paint weight; chase present.

---

## DIAGNOSIS (tonight's measurements — current tip `caf8f33`, Intel UHD 630, headed Chromium)

| test | RASTER (`?gpu=0`) | GPU (`?gpu=1`) |
|------|-------------------|----------------|
| interact-latency, 1 wheel | **121–135ms latency, 2 distinct/scroll — WORKS** | latency **>4000ms**, 0 distinct, 105 frames "pushed" — stale delivery |
| probe-readback-diff, 90 wheels | n/a | forced-readback changed YES, #screen changed YES (only start vs end) |
| gpu-freeze-timeline, 1 wheel | n/a | #screen changed once @113ms then held; **`__bib.readback()` returns NULL every call**; bib.frames stuck while readback polled |

Reading: GPU delivery is **non-deterministic** — the signature of a **cross-thread
delivery race**, not weight and not a clean freeze. Plus a second concrete bug: the async
full-frame readback probe path (`bib_request_readback` → `bib_render_readback`, full
`readPixels`) **returns null in GPU mode**. Raster (same putImageData delivery) is rock
solid → the bug is in the **GPU surface↔readback / cross-thread present**, not delivery
generally.

**Suspicious clue, likely central:** Approach R only works at all with
`explicitSwapControl=true` + `renderViaOffscreenBackBuffer=true`
(PlatformDisplayEmscripten.cpp, `#ifdef __EMSCRIPTEN_PTHREADS__`) + `-sOFFSCREEN_FRAMEBUFFER=1`
(embedder.cmake). Removing them → readPixels reads STALE (frozen frame 1). We never
understood *why* an offscreen **texture** RenderTarget readback needs the default-buffer
preserve. That not-understood coupling is probably the same mechanism as the flaky/stale
delivery. The zero-copy rewrite should make these attrs unnecessary — verify they're gone.

---

## TIMELINE / git anchors

- `557af28` (2026-06-11) decision-005: **MotionMark 109.87 @ 144fps** — main-thread GPU, direct present. THE good state.
- `dc7502b` (06-12 14:58) W-B1: engine → pthread (raster). `33b75fa` (15:26) live Discord chat user-confirmed (raster, pthread).
- `2b96e64` (06-12 18:03) W-B2: GPU under pthread via OffscreenCanvas transfer → commit starvation.
- `dd0766a` (06-12 19:31) "GPU smooth user-confirmed" — but that was the starved transfer path; smooth was INTERMITTENT (the race), not stable.
- `caf8f33` (06-13 01:10) Approach R (readback). Current tip. Flaky — did NOT actually fix the freeze (only made frame 1 render; frame 2+ never verified — my error).

---

## TARGET ARCHITECTURE — present matrix (BIB_PTHREAD × gpu)

Single codebase, three present paths selected at compile time × runtime:

| | **BIB_PTHREAD=ON** (engine on worker; needs SAB+COOP/COEP) | **BIB_PTHREAD=OFF** (engine on main; no SAB) |
|---|---|---|
| **GPU** | worker Ganesh → `OffscreenCanvas.transferToImageBitmap()` → `postMessage(bmp,[bmp])` → main `#screen.getContext("bitmaprenderer").transferFromImageBitmap(bmp)`. ZERO-COPY. | engine creates WebGL2 ctx directly on `#screen` (`getContext("webgl2")`), Ganesh → default FBO, **implicit present**. NO readback/bitmap. = the 109-MotionMark path. |
| **RASTER** | CPU surface → readback → `MAIN_THREAD_ASYNC_EM_ASM` → `#screen.getContext("2d").putImageData()` (today's working path) | CPU surface → `putImageData()` directly (no thread hop) |

**Hard constraint discovered:** a `<canvas>`'s context type is EXCLUSIVE (`2d` vs
`bitmaprenderer` vs `webgl2`, one per element). The host must pick `#screen`'s context by
mode. Switching modes ⇒ recreate the canvas element, or keep mode fixed per page load.
Probe/gate/hello reads (`bib_request_readback`, DOM screenshots) must still work under
`bitmaprenderer` — VALIDATE (bitmaprenderer canvases are still DOM-screenshot-able; engine
readback still reads the SkSurface, independent of host canvas type).

---

## GPT-5.5 DESIGN — host-side context selection (captured; rest in conversation)

**FULL GPT-5.5 design (verbatim, with code for BOTH sides) is saved at
`docs/summaries/gpt55-present-design-2026-06-13.md`** — read it before implementing.
Conversation: https://chatgpt.com/c/6a2cef6b-4740-83ea-b438-2178c46c5c90 (resume with
`gpt55-pro-agent --continue <url> -p "..."`). Host-side `#screen` context selection
(browser.html):

```js
// BIB_PTHREAD_BUILD must be STAMPED AT PACKAGING TIME (not guessed) — e.g. the
// build emits browser-pthread.html (true) / browser-mainthread.html (false),
// or the build script substitutes the constant.
const BIB_PTHREAD_BUILD = true;            // stamped per build
const gpuParam = new URLSearchParams(location.search).get("gpu");
const requestedGPU = gpuParam !== null ? gpuParam !== "0" : !navigator.webdriver;
function supportsBitmapPresent() {         // feature-detect bitmaprenderer
  if (typeof OffscreenCanvas !== "function") return false;
  try { return !!document.createElement("canvas").getContext("bitmaprenderer"); }
  catch { return false; }
}
const gpuMode = requestedGPU && (!BIB_PTHREAD_BUILD || supportsBitmapPresent());
const presentMode = !gpuMode ? "raster-2d" : BIB_PTHREAD_BUILD ? "gpu-bitmap" : "gpu-implicit";
// raster-2d  -> canvas.getContext("2d")  (today's working path)
// gpu-bitmap -> canvas.getContext("bitmaprenderer")  (receives ImageBitmaps from worker)
// gpu-implicit -> DO NOT getContext(); engine calls emscripten_webgl_create_context("#screen")
```

Key point it confirms: **`BIB_PTHREAD` must be visible to the HOST page** (stamped at
build), because the host has to choose the canvas context type before the engine boots, and
context type is exclusive per canvas.

## DESIGN RESOLVED (GPT-5.5) — answers to the open questions

Full code in `gpt55-present-design-2026-06-13.md`. Digest:

1. **Worker→main channel.** Dedicated `MessageChannel`; the raw Emscripten pthread Worker
   postMessage stream is used ONLY for a one-time handshake (NOT `PThread.runningWorkers`):
   - C++ `main()`, after `self.__bibInstallWorkerHooks()`, calls `Module.bibPresentWorkerHello(w,h)`
     → worker does `self.postMessage({__bibPresent:"hello",w,h})`.
   - Main wraps `window.Worker` (subclass, installed BEFORE embedder.js) and listens for that
     exact `{__bibPresent:"hello"}` on each created Worker → creates a `MessageChannel`,
     keeps `port1`, transfers `port2` to the worker via `worker.postMessage({__bibPresentPort:true,port:port2},[port2])`.
   - All frame traffic then rides the MessageChannel. Custom message shapes must NOT contain
     a `cmd` field (avoid colliding with Emscripten's pthread protocol).
2. **transferToImageBitmap timing.** Per frame: paint dirty rects into the persistent texture
   surface → `g_engine->surface->draw(g_fbo0Surface->getCanvas())` (texture→FBO0) →
   `skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get())` → `oc.transferToImageBitmap()` →
   `port.postMessage({t:"frame",bitmap,...},[bitmap])`. Works on the Emscripten GL OC.
   **REMOVE** explicitSwapControl + renderViaOffscreenBackBuffer + `-sOFFSCREEN_FRAMEBUFFER=1`
   for the bitmap path (optionally keep behind a `BIB_GPU_READBACK_PRESENT` legacy define).
3. **Backpressure = strict ONE-IN-FLIGHT (coalesce, never queue).** Worker checks
   `Module.bibBitmapPresentReady()` BEFORE `bibPaintGPUIfDirty()` (which snapshots+clears
   damage); if not ready, return WITHOUT painting → damage stays armed and coalesces. Main
   acks `{t:"ready"}` only after `bitmapCtx.transferFromImageBitmap(bitmap)` returns.
4. **BIB_PTHREAD=OFF main-thread implicit present.** `initializePlatformDisplayEmscripten`
   target becomes `"#screen"` (engine owns the WebGL2 ctx; host does NOT getContext); same
   two-surface structure (texture RT + FBO0 wrap); present = draw texture→FBO0 + FlushAndSubmit
   + return to event loop (WebGL auto-presents; `GLContext::swapBuffers()` already a no-op).
   `installGpuContextLossHandlers()` must branch: `GL.offscreenCanvases["bibgpu"]` (pthread)
   vs `document.getElementById("screen")` (mainthread).

**Crucial refinement over my earlier insight:** do NOT make WebCore paint *directly* into
FBO 0 as the persistent store (FBO 0 is undefined after composite w/ preserveDrawingBuffer
off → would force full-viewport repaint every frame). KEEP the texture RenderTarget as the
persistent dirty-rect backing store, and do a present-time GPU→GPU **blit** texture→FBO0
(`g_engine->surface->draw(g_fbo0Surface->getCanvas())`) before transferToImageBitmap.

## MY ASSESSMENT / TEST-FIRST FLAGS (don't trust blind — crosscheck during impl)

- **`window.Worker` subclass hook**: must be installed before embedder.js AND survive
  Emscripten's PTHREAD_POOL_SIZE pre-spawn. Verify the engine (proxied-main) worker is
  created through the wrapped constructor and that `Object.setPrototypeOf`+`prototype` reassign
  doesn't break Emscripten's worker bootstrap. If it does, fall back to: engine `self.postMessage`
  hello + main listens via a tiny patch in the Emscripten worker (engine-pre.js can also
  `self.postMessage` and main can use `Module.PThread` as a last resort).
- **Vertical flip**: texture surface is `kTopLeft_GrSurfaceOrigin`, FBO0 wrap is
  `kBottomLeft`. The texture→FBO0 `draw()` + transferToImageBitmap + bitmaprenderer chain is
  a classic flip trap — eyeball the first frame; if flipped, flip an origin or the draw matrix.
- **transferToImageBitmap on an Emscripten GL OffscreenCanvas with preserveDrawingBuffer=false**:
  should capture the current frame, but VERIFY frame 2+ aren't blank (that was the whole bug).
- **Probe/gate path**: keep readback ONLY for `bib_request_readback` (gates/hello). It now
  force-paints the texture surface then reads it (independent of the bitmap present). Confirm
  gate8 still green and DOM screenshots of a `bitmaprenderer` #screen still work.

---

## KEY IMPLEMENTATION INSIGHT (confirmed + refined by GPT-5.5)

`transferToImageBitmap()` snapshots the OffscreenCanvas's **default framebuffer (FBO 0)**.
But today the engine renders into a **texture-backed `SkSurfaces::RenderTarget`**
(`g_engine->surface`, main.cpp ~1848) and reads THAT back — it never draws to FBO 0 in
steady state (`g_fbo0Surface` exists but is only used for the boot software-detect bench).
That mismatch is why the readback was flaky and why `renderViaOffscreenBackBuffer`+
`preserveDrawingBuffer` were "load-bearing". **GPT-5.5's refinement (do this, NOT paint
into FBO 0 directly):** keep the texture RT as the persistent dirty-rect backing store, and
at PRESENT time blit it to FBO 0 — `g_engine->surface->draw(g_fbo0Surface->getCanvas())` +
`FlushAndSubmit(g_fbo0Surface)` — THEN `transferToImageBitmap()` the bibgpu OC. (Painting
WebCore straight into FBO 0 would force a full-viewport repaint every frame since FBO 0 is
undefined after composite with preserveDrawingBuffer off.) The preserve-buffer hack then
goes away. The boot GPU path (main.cpp ~1636-1681) registers the
bibgpu OffscreenCanvas and calls `initializePlatformDisplayEmscripten("#bibgpu")`; for
BIB_PTHREAD=OFF that becomes `"#screen"` and FBO 0 IS the visible canvas (implicit present).

## IMPLEMENTATION STEPS (refine after GPT-5.5)

1. **Host (browser.html):** pick `#screen` context by mode — `bitmaprenderer` (pthread GPU),
   `webgl2`-owned-by-engine (mainthread GPU), `2d` (raster). Add the bitmap receiver
   (`port.onmessage` → `transferFromImageBitmap`). Keep raster path intact.
2. **Worker (engine-pre.js):** establish the main channel (per GPT-5.5); expose
   `Module.bibPresentBitmap()` that transferToImageBitmap's the bibgpu OC and posts it.
3. **Engine (main.cpp):** in GPU+pthread, replace the per-frame FlushAndSubmit+readPixels+
   bibPushFrameIfDirty with FlushAndSubmit + `bibPresentBitmap()`. Keep readback ONLY for
   the probe/gate path. Restore mainthread-GPU implicit present under BIB_PTHREAD=OFF.
4. **Port (PlatformDisplayEmscripten.cpp):** drop explicitSwap/renderViaOffscreenBackBuffer
   if the zero-copy path doesn't need them; for BIB_PTHREAD=OFF target "#screen" directly.
5. **embedder.cmake:** drop `-sOFFSCREEN_FRAMEBUFFER=1` if unused; ensure flags are
   BIB_PTHREAD-gated correctly.
6. **Verify:** gate8 (GPU) + gate2 (raster) green; interact-latency GPU now <200ms reliable;
   re-run MotionMark (user) for both BIB_PTHREAD on/off; Codex review; export WebKit patch.

---

## TOOLS (build/, gitignored; created/used this session)

- **`build/interact-latency.mjs [url] [gpu]`** — THE present truth-teller: wheel→first
  visible pixel-change latency + bib.frames + console buckets. GPU>4000ms vs raster ~130ms
  is how this whole thing was caught.
- **`build/perflog-hardscroll.mjs [url] [gpu]`** — drives wheel as fast as possible
  (oscillating mid-page) so BIBPERF/s shows the real paint ceiling, not the test's input
  rate. (The original perflog-capture.mjs under-drives at ~8/s — misleading.)
- **`build/probe-readback-diff.mjs [url] [gpu]`** — forced-readback hash + #screen sha,
  before vs after scroll. Separates "surface stale" from "push path stale".
- **`build/gpu-freeze-timeline.mjs [url] [gpu]`** — single wheel, polls #screen + forced
  readback every 250ms. Showed the non-determinism + the readback=null bug.
- `?perflog=1` (engine BIBPERF/s per-phase timing) still in main.cpp — keep.
- Run headed: `DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/<tool> <url> <gpu>`.

---

## CURRENT TREE STATE

- Tip = `caf8f33` (Approach R readback). Working tree CLEAN (the raster-default band-aid I
  briefly added to browser.html was reverted — user wanted the real fix, not a default flip).
- Binary in build/webcore/bin = Approach R (flaky GPU). Servers dev:8080 + wisp:5001 up.
- task #74 in_progress (the present problem). #57 (toObjectSlow) still open separately.

## STANDING STATE / build

- `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0 --collect -- env BIB_JOBS=6 bash tools/build-webcore.sh`
  (incremental). BIB_PTHREAD default ON. WebKit-tree edits → `bash tools/export-webkit-patches.sh` before commit.
- COOP same-origin + COEP require-corp required for BIB_PTHREAD=ON. BIB_PTHREAD=OFF is the no-SAB escape (and the user wants it to be a real, supported, fast-GPU config).
