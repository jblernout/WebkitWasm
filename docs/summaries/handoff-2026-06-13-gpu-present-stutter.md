# Handoff — GPU present stutter follow-up (zero-copy shipped, pacing regression open)

**Written**: 2026-06-13 ~12:10 EDT. **Active handoff.** Supersedes
`handoff-2026-06-13-gpu-present-rearchitecture.md` (that one is the IMPLEMENTED
plan; read it for the present-architecture details — present matrix, design,
GPT-5.5 doc pointer). Read THIS first.

---

## ✅ RESOLUTION (2026-06-13 PM) — pacing fixed (2×), root cause reframed

**Shipped + committed:** the strict one-in-flight present was replaced with a
**triple-buffer credit (`maxInFlight=3`) + ack-on-receive** (page acks BEFORE
compositing). New `?inflight=N` debug knob (page→worker config msg, default 3,
1 = legacy). engine-pre.js + browser.html. Codex-clean (5/5 non-issue),
**gate8 PASS, gate2 PASS** (no raster regression).

**Validated by same-binary A/B on a SATURATED page** (Discord GPU, `?inflight`):

| inflight | mean fps | cv (var) | worst stall |
|---|---|---|---|
| 1 (legacy) | 6.6 | 2.41 | 2150ms |
| **3 (fix)** | **13.8** (2.1×) | 1.69 | 1750ms |
| 6 | 15.4 | 1.79 | 1350ms |  ← diminishing returns; knee at 3

**Root cause was TWO problems conflated:**
1. **Present pacing (FIXED, 2×).** The one-in-flight ack serialized the engine
   behind every present round-trip. *Only visible under saturation* — the light
   isolator (`web/probe/hardgfx.html`, 135fps cv0.12) shows `inflight=1`≡`3`
   because there's no long task to recover from. The first light-isolator A/B
   was the WRONG test; the saturated A/B proved the win.
2. **Engine-thread saturation (DOMINANT, not a present bug).** The single engine
   pthread runs guest JS (no-JIT CLoop) AND canvas/layer raster serially; a heavy
   frame blocks it 0.1–2s (BIBPERF: `renderUpd=1008ms`, `runloop(JS)` multi-sec)
   → no `bib_tick` → present halts → freeze. Hits BOTH modes (raster worse:
   Discord raster=0.8fps). Present pacing cannot fix it.

**Pre-pthread question (why main-thread GPU never stuttered):** the *slowness* is
fundamental (no-JIT+single-thread), but the *bursty freeze texture* is largely
**pthread-specific** — main-thread used IMPLICIT present (browser auto-composites
#screen's webgl2; graceful fps ramp-down under load), whereas pthread needs a
cross-thread present (FlushAndSubmit + transferToImageBitmap sync + postMessage +
tick-proxy) that degrades burstily. The fix removed the ack-round-trip slice;
the rest is inherent to off-main-thread present. **Full restoration = BIB_PTHREAD
=OFF** (Fix B) — tradeoff: guest heavy-JS freezes the host tab.

**Residual direction chosen (user):** (1) document as fundamental [done, here]
+ (3) investigate GPU-accelerated canvas2D ImageBuffers (shadowBlur = ~1s/frame
on the engine thread → canvas2D appears CPU-rasterized even in GPU mode; GPU-
backing it would cut canvas-heavy saturation). Fix B (BIB_PTHREAD=OFF) NOT done
this round but remains the definitive lever for the pthread burstiness.

**Tools added:** `build/present-cadence.mjs` (page-side `__bib.frames` cadence
referee — `screencast-fps.mjs` is DEAD under Xvfb, reads ~0 for both modes),
`web/probe/hardgfx.html` (`?shapes=N` load-tunable present isolator).

The rest of this doc is the PRE-resolution investigation plan (historical).

---

## TL;DR

1. **Zero-copy GPU present is implemented + verified + WORKS** (frozen→interactive):
   gate8 + gate2 green, MotionMark **103** (old main-thread peak was 109), headed
   real-GPU latency 76–388ms. Details in the rearchitecture handoff.
2. **NEW OPEN ISSUE (a pacing regression, NOT yet fixed): the GPU present path
   STUTTERS + randomly freezes on hard-graphics pages** (Discord homepage,
   MotionMark) — *despite the same MotionMark score*. User: "smooth for a sec then
   lock up to 1fps for a few sec," "random freezing," "never recall it doing it
   before."
3. **Isolated by A/B (user-confirmed):** `?gpu=0` (raster) is **smooth but slow**;
   `?gpu=1` (gpu-bitmap) **stutters**. So it is the **present path**, NOT engine
   (JS/GC/renderUpd hit raster too, and raster is smooth).
4. **GC is RULED OUT** (measured via the new `?gclog=1` gate): JSC full
   collections are **2–6ms**, Eden ~1ms — negligible. Not the cause.
5. **Everything is UNCOMMITTED** — the present rewrite + the stutter is unresolved,
   so do NOT commit until the stutter is addressed or the user decides. Working
   tree files listed below; on-disk binary = the fixed present + `?gclog` gate.

---

## THE OPEN PROBLEM — GPU present stutter on hard graphics

**Symptom:** hard-graphics pages render at high throughput (MotionMark 103) but the
*visible* delivery hitches — smooth ~1s, then a multi-second ~1fps lock, repeating.
Raster (`?gpu=0`) never does this (it's just uniformly slow). Predates nothing the
user remembers being this bad — i.e. a **regression vs the pre-W-B main-thread GPU
era** (which presented implicitly, no cross-thread handoff).

**Why it's the present path, not the engine (evidence):**
- `?gpu=0` raster = smooth-but-slow; `?gpu=1` = stutter. Same engine/JS/renderUpd
  underneath → the delta is purely *delivery*.
- `?gclog=1` GC trace (`/tmp/gc-trace-discord.log`): full GCs 2–6ms — not it.
- BIBPERF on Discord steady-state showed engine *painted* 38–53fps (engine is
  keeping up); the stutter is in getting those frames to the glass.

**Leading hypothesis (medium-high):** the GPU present is **strict one-in-flight with
an ack round-trip** — the engine BLOCKS (`bibBitmapPresentReady()` false →
`bibPushFrameIfDirty` returns without painting) until the page acks each frame.
Raster is **fire-and-forget** (`MAIN_THREAD_ASYNC_EM_ASM` → `bibBlit`, no ack, engine
never waits). On hard graphics the page main thread jitters (compositing the
ImageBitmap, GPU contention), so the ack jitters, so the engine serializes into
visible hitches. The old smooth path had no handoff at all.

**Secondary hypothesis (also plausible, check if the fix below underperforms):**
**GPU cross-context contention.** The worker owns a Ganesh WebGL2 context on the
`#bibgpu` OffscreenCanvas; the page composites a *different* context
(`bitmaprenderer` on `#screen`); `transferToImageBitmap`/`transferFromImageBitmap`
hand a GPU-backed bitmap across contexts — all on ONE Intel UHD 630. Under
MotionMark load the worker render + the cross-context handoff + the page compositor
all contend for the single GPU → periodic stalls. The main-thread path used ONE
context (the canvas itself), so no handoff/contention.

---

## FIX PLAN (next session) — do in this order

### A. CHEAP, try first — relax the backpressure (relink only, ~5 min)
Make GPU delivery non-blocking like raster. Change the strict one-in-flight gate to
**"latest-wins" / N-in-flight** so the engine never stalls on the page ack:
- `web/engine-pre.js`: replace the boolean `ready` with an `outstanding` counter;
  `bibBitmapPresentReady()` returns `outstanding < MAX` (try MAX=2, then 3). Increment
  on post; the page acks each displayed frame to decrement.
- `web/browser.html` receiver: on each frame `transferFromImageBitmap` immediately;
  if frames arrive faster than display, **close the superseded ImageBitmap** (latest
  wins — never queue stale frames) and ack so `outstanding` drops.
- Keep the bound (MAX) so GPU-backed ImageBitmaps can't accumulate (memory).
- Rebuild: `touch src/embedder/main.cpp` then the standard build (pre-js isn't
  CMake-tracked). Re-verify: gate8, then headed MotionMark/Discord by eye + the
  visible-fps referee below.

### B. DEFINITIVE smooth path — build BIB_PTHREAD=OFF (full rebuild, ~1.5–2h, separate build dir)
This is the **gpu-implicit, main-thread** config — engine owns `#screen`'s webgl2
context directly, browser presents implicitly, NO cross-thread handoff = the old
smooth 109 path the user remembers. Implemented but **never built/verified** this
session. Build: `BIB_PTHREAD=0 BIB_JOBS=6 bash tools/build-webcore.sh` (use a
separate build dir — flag change recompiles the whole tree). The host auto-selects
`gpu-implicit` because `bib-build-config.js` will stamp `BIB_PTHREAD_BUILD=false`.
Trade-off: guest long JS tasks freeze the host tab (no worker isolation). Right
config for pure-graphics / benchmarking; ON build is right for JS-heavy real apps.

### C. If A underperforms — investigate GPU cross-context contention
Measure the page-side cost: instrument the `browser.html` bitmap receiver to log
`transferFromImageBitmap` duration + inter-frame gap. If that spikes while engine
BIBPERF is steady → it's the handoff/compositor, not backpressure. Options: reduce
present resolution/format, avoid the cross-context copy, or accept B for graphics.

---

## NEXT DIAGNOSTIC (was mid-run when we stopped to write this)
Confirm "engine painted steady but visible bursty" = delivery stutter:
```
DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/screencast-fps.mjs https://discord.com 1   # GPU
DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/screencast-fps.mjs https://discord.com 0   # raster A/B
```
`screencast-fps.mjs` = TRUE visible fps via CDP screencast vs engine `painted=`.
Expect GPU: high painted, bursty/low visible. Raster: low but even visible.
(MotionMark is the user's sharpest repro but harder to drive headless — needs a
Run-button click via Playwright.)

---

## WHAT'S IN THE TREE (all UNCOMMITTED — do not commit until stutter resolved/decided)
Tracked (in `git status`):
- `src/embedder/main.cpp` — present rewrite (bibPaintGPUIfDirty, presentGPUToCanvasFBO,
  bibBitmapPresentReady/bibTransferCurrentFrameBitmap, 3-branch bibPushFrameIfDirty,
  GPU bib_render_readback, boot target + loss-handler pthread/mainthread split,
  worker→main hello) **+ the new `?gclog=1` gate** (JSC_logGC).
- `web/engine-pre.js` — worker present bridge (worker creates MessageChannel, transfers
  port2 in the hello; postMessage-throw guard from Codex).
- `web/browser.html` — 3-mode context select, `window.Worker` subclass + bitmap receiver
  + one-in-flight ack, readback-based hello-gate judge for non-raster.
- `src/embedder/embedder.cmake` — dropped `-sOFFSCREEN_FRAMEBUFFER=1`; `file(GENERATE)`
  emits `bib-build-config.js` (stamps `window.BIB_PTHREAD_BUILD`).
- `src/patches/webkit-emscripten.patch` — re-exported; gates explicitSwapControl +
  renderViaOffscreenBackBuffer behind legacy `BIB_GPU_READBACK_PRESENT`.
- `docs/summaries/handoff-2026-06-13-gpu-present-rearchitecture.md` — marked IMPLEMENTED.
Git-ignored but edited (captured in the patch above):
- `third_party/WebKit/Source/WebCore/platform/graphics/emscripten/PlatformDisplayEmscripten.cpp`
Untracked leave-alone: `web/probe/wasmrepro/`.

**On-disk binary** (`build/webcore/bin/embedder.{js,wasm}`) = present-fix +
postMessage-fix + `?gclog` gate, **BIB_PTHREAD=ON**. `bib-build-config.js` present
(stamps true). Servers up: dev :8080, wisp :5001.

---

## VERIFICATION / TOOLS
- `node tools/gate8-gpu-test.mjs` (GPU pipeline + G4 + software-fallback) — was PASS.
- `node tools/gate2-browser-test.mjs` (raster pixel-exact) — was PASS.
- `DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/interact-latency.mjs <url> 1` — wheel→pixel latency.
- `... build/perflog-hardscroll.mjs <url> 1` — BIBPERF/s per-phase (present/paint/runloop/renderUpd).
- `... build/gc-trace.mjs <url>` (new) — correlates BIBPERF stalls with JSC GC log (`?gclog=1`).
- `... build/screencast-fps.mjs <url> <gpu>` — TRUE visible fps vs engine painted (the present referee).
- Build: `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0 --collect -- env BIB_JOBS=6 bash tools/build-webcore.sh`
  (incremental relink for embedder/pre-js/cmake; full rebuild only on compile-flag change e.g. BIB_PTHREAD).
  WebKit-tree edits → `bash tools/export-webkit-patches.sh` before commit.
- ⚠️ Background builds peg 6 cores and visibly worsen the engine stutter while running
  — don't benchmark/eyeball the engine while a build runs.

## KEY FACTS / DECISIONS
- Codex review of the present rewrite: 2 findings — stale-runtime (fixed by rebuild),
  postMessage-throw leak/wedge (fixed). Everything else clean.
- Handshake deviation from GPT-5.5's doc (intentional): WORKER creates the
  MessageChannel + transfers port2 in the hello (worker→main only) — nothing custom
  crosses Emscripten's worker onmessage.
- GC RULED OUT as the stutter cause (2–6ms full GCs). The boot locks on Discord are
  `runloop(JS)` (no-JIT guest JS, up to ~1.9s) — transient/fundamental; the
  *hard-graphics* stutter is the present path (this handoff's problem).
- Present cost (engine-side `presentGPUToCanvasFBO` = texture→FBO0 blit +
  FlushAndSubmit) ≈ 1.4–3.9ms/frame in steady scroll — not the stall, but it's a
  per-frame GPU sync on the engine thread that the main-thread path didn't have.

## TASKS
- #74 (GPU present re-architecture) — present is implemented/verified; KEEP OPEN for
  the stutter follow-up, or split a new task "GPU present pacing: stutter/freeze on
  hard graphics (one-in-flight → latest-wins / BIB_PTHREAD=OFF)".
- #57 (Discord login toObjectSlow abort) still open. Discord also throws a pre-existing
  guest `memory access out of bounds` (engine survives) — unrelated to present.
