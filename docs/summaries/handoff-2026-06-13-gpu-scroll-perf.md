# Handoff — GPU scroll/interaction is low-fps & laggy (the REAL problem)

**Written**: 2026-06-13 ~00:50 EDT. Supersedes handoff-2026-06-12-discord-perf-regression.md
(that one's open questions are now mostly ANSWERED — see below). Read this before touching perf.

---

## THE PROBLEM (user's exact framing, 2026-06-13)

> "even just scrolling it feels like shit … initial load is fine that never was the
> problem its just afterwards … low fps and laggy when it used to not be"

- **Initial load / first render: FINE.** Never the problem. (My screencast "0.2 visible fps"
  panic was partly the frozen-render bug, now fixed — see FIXED.)
- **The actual problem: scroll & in-app interaction are low-fps / laggy AFTER load.**
- **Load-bearing user claim: GPU scroll "used to not be" laggy — it regressed.** Not reproduced
  in the current (W-B1/W-B2 pthread) architecture this session; strongest hypothesis is it was
  the **pre-W-B1 G3 main-thread-GPU era** (see HYPOTHESES). UNRESOLVED — answer this first.

---

## FIXED THIS SESSION (real, keep — but did NOT fix the scroll-weight problem)

The GPU path was rendering ~47fps engine-side but only **~0.2fps reached the screen** —
**OffscreenCanvas placeholder commit starvation** under `-sPROXY_TO_PTHREAD` (the engine
thread's rendering-opportunity that flushes the placeholder commit is starved). Confirmed by
measuring engine-painted vs CDP-screencast visible fps (build/screencast-fps.mjs), corroborated
by the user's eyes ("near-frozen").

- `emscripten_webgl_commit_frame()` is a **confirmed no-op** for this case (browsers removed
  `OffscreenCanvas.commit()`; with `renderViaOffscreenBackBuffer` it only blits the offscreen
  backbuffer, not the placeholder). Do NOT revisit it.
- **Fix shipped ("Approach R"):** stop using the OffscreenCanvas placeholder entirely. GPU now
  renders into a **worker-private OffscreenCanvas** ("#bibgpu", registered in
  `GL.offscreenCanvases`), the engine **reads the frame back to CPU and delivers it via the
  SAME readback+putImageData path raster uses** (host owns #screen's 2d context). `#screen` is
  no longer transferred. Verified: **GPU renders Discord homepage correctly** (build/shot.mjs
  screenshot — logo, login, 3D art, gradient all correct).
- **Sub-bug found & fixed:** Approach R first shipped STALE pixels (engine painted 60fps,
  screen static) because `readPixels` ran before the GPU executed the recorded Ganesh paint.
  Fix: `skgpu::ganesh::FlushAndSubmit(g_engine->surface.get())` before the GPU readback.

Net: GPU went from **near-frozen/stale → renders correct content & delivers frames**. But
**scroll still feels bad** (user-confirmed), so the headline problem below is untouched.

---

## THE OPEN PROBLEM: scroll/interaction is weight-bound (low fps), not frozen

Per-phase engine-thread timing (NEW instrument: `?perflog=1` → "BIBPERF/s" lines; see TOOLS).
Measured on discord.com, quiet-ish machine, during an active scroll:

- **Raster (gpu=0):** ~8 fps. **PAINT-dominated** — ~111ms per FULL-VIEWPORT frame of Skia CPU
  raster. Scroll dirties the whole viewport, so dirty-rect buys nothing → full 793×600 CPU
  raster every frame.
- **GPU (gpu=1, Approach R):** paint itself is fast (Ganesh ~5ms) but per-frame total is still
  high. During scroll, the big costs were **renderUpd ≈ 470ms/s** (updateRendering +
  finalizeRenderingUpdate = style recalc + rAF callbacks + IntersectionObserver) **+ paint
  ~270ms/s + present** — AND Approach R now ADDS a **per-frame full-viewport GPU→CPU readback +
  FlushAndSubmit stall** on top. So GPU scroll is bound by renderUpd + layout + readback, not
  by paint.
- **Boot is separately JS-bound** (Discord bundle at JIT-less CLoop speed; single
  RunLoop::cycle macrotasks of 1.0–1.6s). Real, fundamental, but NOT the scroll problem.

So the scroll problem is **per-frame WEIGHT** (style/rAF + layout + paint/readback), not a spin
loop, not network, not the frozen-commit bug. It is the cost of running Discord's scroll path
(heavy JS scroll handlers + full-viewport repaint) in a JIT-less engine, now plus a readback.

---

## HYPOTHESES (for next session, ranked)

1. **The "used to be smooth" GPU was the pre-W-B1 G3 MAIN-THREAD era.** Pre-W-B1: GPU on the
   main thread, direct implicit WebGL present — NO pthread proxy hops, NO readback, NO commit
   hop. W-B1/W-B2 moved the engine to a pthread (proxying overhead) and Approach R added a
   per-frame readback. These structurally LOWER the interactive ceiling. **Test:** build the
   pre-W-B1 G3 commit and A/B scroll on a QUIET machine. If G3 main-thread GPU scrolls smooth
   and W-B2 doesn't, the regression is architectural (the pthread move), not a stray commit.
2. **Approach R's per-frame full-viewport readback is a real new cost.** The zero-copy path
   that AVOIDS readback is `transferToImageBitmap → MessagePort.postMessage → host
   bitmaprenderer` — **validated by GPT-5.5 Pro this session** but NOT implemented: the hard
   part is plumbing a MessagePort/transferable from the emscripten PROXY_TO_PTHREAD engine
   worker to the host main thread (worker `self.postMessage(bmp,[bmp])` + host
   `addEventListener` on the pthread's Worker object, OR a MessageChannel port plumbed into the
   worker via engine-pre.js). GPT-5.5 conv: https://chatgpt.com/c/6a2cd629-08a4-83ea-9b38-d1658716ca23
   (the CLI only captured its final diagram; full prose is behind ChatGPT auth — re-fetch with
   `gpt55-pro-agent --continue <url>` if needed).
3. **renderUpd (~470ms/s on GPU scroll) may be the dominant, attackable cost.** style recalc +
   rAF + IntersectionObserver per scroll frame. Worth a finer breakdown (split updateRendering
   from finalizeRenderingUpdate; see if IntersectionObserver/JS is the bulk).
4. **Scroll blit-shift (task #42) is NOT engaging on Discord** (full-viewport repaint every
   scroll frame). Discord uses a transformed/virtualized scroll container, not document scroll,
   so WebCore repaints everything. If we could detect & blit-shift Discord's scroll, paint+
   readback would drop ~10×. Hard (custom scroller), but high-leverage.

## OPEN QUESTIONS (answer in order)

1. Quiet-machine, GPU: get a clean BIBPERF/s scroll breakdown (build/perflog-capture.mjs) and
   say definitively which phase dominates: renderUpd vs layout vs paint vs readback.
2. Was GPU scroll EVER smooth in the pthread era? Bisect vs pre-W-B1 G3 (hypothesis 1).
3. Is the readback (hypothesis 2) a big enough slice to justify the transferToImageBitmap
   rework? (Measure readback ms/frame specifically — add a perf bucket around the GPU readPixels.)
4. Can renderUpd be cut (hypothesis 3)?

---

## TOOLS (build/, gitignored, all created this session)

- **`?perflog=1`** (host URL) → engine emits **"BIBPERF/s"** once/sec: per-phase ms breakdown
  (runloop[JS] / renderUpd / layout / paint / present / pushOther / persist) + busy% +
  avgPaintedFrame. THE instrument for "where does the frame go". (main.cpp, gated, ~zero cost off.)
- **`build/perflog-capture.mjs [url] [gpu]`** — loads, settles, scrolls, prints BIBPERF/s.
  `DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/perflog-capture.mjs https://discord.com 1`
- **`build/screencast-fps.mjs`** — TRUE visible fps via CDP screencast vs engine-painted.
  ⚠ UNRELIABLE in an occluded/unfocused headed window (Chrome throttles screencast) — it
  under-reported Approach R as 0.2fps when the screen was actually rendering. Use
  screenshot-distinct for occlusion-independent truth.
- **`build/interact-latency.mjs [url] [gpu]`** — bib.frames rate (engine→host blits) +
  screenshot-distinct visible fps + scroll latency + console-bucket frequency (spin-loop tell).
  screenshot capture is ~7–18/s capped, so it UNDERCOUNTS true fps; bib.frames is the trustworthy
  engine-side counter.
- **`build/shot.mjs <gpu> <tag>`** — grabs #screen settle+scrolled PNGs to LOOK at actual output.
- Lesson reinforced: **engine-painted ≠ visible fps ≠ smooth.** Always cross-check
  bib.frames (engine push) AND a pixel method AND the user's eyes.

---

## CURRENT TREE STATE (uncommitted — main @ 020df69)

Working tree has, NOT yet committed:
- **perflog instrumentation** (main.cpp): PerfAccum + bibNowMs + BIBPERF/s in bib_tick/bib_render
  + `?perflog=1` read. KEEP — it's the key instrument.
- **Approach R** (main.cpp + web/browser.html): worker-private #bibgpu OffscreenCanvas, GPU
  readback+FlushAndSubmit, no #screen transfer, host ctx always 2d, hello-judge via bibBlit,
  G4 loss handler retargeted to "#bibgpu". This is a STRICT improvement (frozen→rendering).
- **`explicitSwapControl`+`renderViaOffscreenBackBuffer` (PlatformDisplayEmscripten.cpp, a
  WebKit-tree edit → src/patches/webkit-emscripten.patch) + `-sOFFSCREEN_FRAMEBUFFER=1`
  (embedder.cmake) are LOAD-BEARING — do NOT remove.** They were originally added for the
  abandoned commit_frame present, but it turns out they force `preserveDrawingBuffer=true`, which
  Approach R's `readPixels` NEEDS: reverting them (tried 2026-06-13) made the GPU read back STALE
  pixels → screen froze on frame 1 (boot test page). Mechanism not fully understood — likely
  g_engine->surface in GPU mode is fbo0-backed (or shares the default buffer), so without
  preserve the painted content is discarded before readback. **Next-session: understand WHY, then
  decide if a cleaner preserve mechanism exists.** The dead `presentGPU()` + `<emscripten/html5.h>`
  include WERE safely removed (uncalled). `g_fbo0Surface` retained for the boot bench.
- Binary currently built (build/webcore/bin) = Approach R + flush fix, gpu renders correctly.
- Gates NOT yet re-run on Approach R (gate8 GPU / gate2 raster) — DO before committing.

Codex verified Approach R logic (1 HIGH = the loss-handler "#screen"→"#bibgpu", fixed; 1 LOW =
bibGpuFallback reload now reload-instead-of-in-place, acceptable).

## STANDING STATE

- GPU default = !navigator.webdriver. ?gpu=0 raster, ?gpu=1 GPU, ?perflog=1 timing.
- Servers: dev :8080, wisp :5001 (restart if stale). Build:
  `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0 --collect -- env BIB_JOBS=6 bash tools/build-webcore.sh`
  (incremental; only changed TUs + relink). BIB_PTHREAD defaults 1.
- task #73 (this fix) in_progress; #72 (triage) done.
