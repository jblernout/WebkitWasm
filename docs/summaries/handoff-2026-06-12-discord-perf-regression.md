# Handoff — Discord steady-state perf regression (near-frozen render)

**Written**: 2026-06-12 ~23:15 EDT, end of a long session. Purpose: hand the
Discord "almost frozen after load" investigation to next session with a
clean separation of what is **measured fact** vs **user-reported** vs
**assumption/maybe** vs **open question**. Read this BEFORE touching perf.

The crash + freeze bugs from this session ARE fixed (see "Fixed this
session"); the OPEN problem is steady-state render rate on heavy-JS pages.

---

## THE OPEN PROBLEM (one line)

On the current binary, Discord renders at **~0.3 visible fps** (GPU) /
**~1.6 fps** (raster) — effectively frozen — while a simple animation
renders at a smooth 60fps. User reports GPU mode "used to have good perf"
on Discord and it regressed; I could not reproduce a fast-Discord state.

---

## FACTS (measured by me this session, this machine: Intel UHD 630, headed real GPU)

- **VISIBLE frame rate** (measured by screenshotting `#screen` rapidly and
  counting distinct frames — `build/visible-fps.mjs`, `build/gpu-visible.mjs`):
  - hello demo (trivial JS), GPU: **8/8 distinct → ~60 fps, SMOOTH**.
  - discord.com homepage, GPU: **1 distinct frame in 3.1s → ~0.3 fps**.
  - discord.com homepage, raster: **5 distinct in 3.1s → ~1.6 fps**.
  - ⇒ The RENDERER/present pipeline is healthy (hello proves it). Discord
    specifically is starved. Same machine load for both → discord-specific.
- **Guest rAF rate is NOT visible fps.** Engine reports `raf-rate 60` on
  discord.com GPU while the screen shows 0.3 fps. Every earlier
  "raf-rate 94/60" number measured guest-side callback firing, NOT what
  reaches the screen. Do not trust raf-rate / hostTicks for smoothness;
  use the screenshot-distinct method.
- **GPU present itself is cheap**: present-bench 2–3 ms/frame on the UHD 630
  (warmed, 8x overdraw). Not the bottleneck.
- **No render-path code changed** between the build the user last called
  "smooth-ish" (babf7fc, the rAF nudge, ~19:30) and now. Commits since:
  705d02f (wasm bridge null-check + multi-table reject), d95b601 + 020df69
  (JSCell toObjectSlow recovery + log rate-limit). None touch paint/present.
  `git diff babf7fc..HEAD -- src/embedder/main.cpp web/browser.html` to confirm.
- **Machine was under heavy continuous build load ALL session** (6-core
  builds every few minutes; load avg peaked ~6.2, was 4.25 at measure time).
  This is a real confound for any timing taken tonight. BUT: hello hit 60fps
  at that same load, so machine load alone does not explain Discord's 0.3.
- libdiscore.wasm = 1.17MB, **2 defined tables** (table section id 4 count=2),
  confirmed via byte parse. binaryen wasm2js took **58.8s** synchronously on
  the engine thread before its multi-table FATAL (measured 58784ms). Now
  rejected in **10ms** (705d02f). Spike data: binaryen translate time is
  module-SHAPE-bound not size-bound (a 1.09MB module = 109s; a 9MB = 0.7s).

## USER-REPORTED (not independently reproduced by me)

- **GPU (non-raster) mode "used to actually have good perf" on Discord** and
  has regressed to near-frozen. ← The load-bearing claim. I have NEVER
  measured Discord itself rendering fast in the CURRENT (W-B1 worker +
  OffscreenCanvas) architecture — only hello. So either (a) it was fast
  earlier today and a non-render change starved it, or (b) "good perf" was
  the pre-W-B1 G3 main-thread-GPU era (different architecture). UNRESOLVED.
- It "got worse sometime today," and the rAF nudge (babf7fc) "partially
  fixed it" — Discord homepage went from totally frozen (1 frame/min,
  pre-nudge OffscreenCanvas commit starvation) to "smooth again ish." So
  "smooth-ish" may have been *relative to 1-frame/min*, not true 60fps.
- Animations looked visually laggy even in my headed measurement window
  (consistent with the 0.3 fps measurement).
- Earlier in the session the user found raster "worse" than GPU on the app;
  tonight's measurement shows raster (1.6) slightly better than GPU (0.3) on
  the marketing homepage. CONFLICTING across time/page — treat as unreliable
  until re-measured on a quiet machine.

## ASSUMPTIONS / STRONG INFERENCES

- **0.3 fps = the engine thread is starved by Discord's JS** (JIT-less CLoop)
  so it rarely reaches paint/present. Evidence: hello (light JS)=60fps,
  discord (heavy JS)=0.3fps, same renderer/machine. STRONG but not proven to
  be "weight" vs "spin".

## MAYBES (explicitly unproven — test next session)

- 0.3 fps SUSTAINED 8s after load *smells like a spin/retry loop* (Discord
  gateway reconnect, the repeated `[PostMessageTransport] Protocol error:
  event data should be an Array!`, failed-resource retries, an unsupported-API
  throw-loop) rather than settled heavy JS. A settled marketing page should
  idle. NOT MEASURED — need engine-thread CPU-over-time / phase timing.
- The rAF nudge (worker rAF per present) and/or `FlushAndSubmit`'s GPU sync
  per present MIGHT block/churn the engine thread under load. Not isolated.
- "GPU used to be good" MIGHT be the G3 main-thread era (pre-W-B1, commit
  before 33b75fa lineage): main-thread WebGL, no pthread proxy, no
  OffscreenCanvas commit hop. If so, the current worker architecture may have
  a structurally lower ceiling and "regression" = the architecture change,
  not a recent commit.
- Machine build-load MIGHT have depressed tonight's numbers somewhat.

## OPEN QUESTIONS (answer these first next session)

1. **Spin loop or heavy-JS?** Instrument bib_tick / the engine RunLoop to log
   time spent in JS exec vs layout vs paint vs present per second on
   discord.com. If the thread is ~100% in JS continuously after load → spin
   loop (findable, fixable). If bursty then idle → weight.
2. **Was Discord EVER fast in the W-B1 worker architecture?** Bisect: the
   user's "good GPU perf" memory — did it predate W-B1 (33b75fa)? If yes, the
   ceiling is architectural. Consider building 33b75fa (or the G3 main-thread
   build) for an A/B on a QUIET machine.
3. **Quiet-machine re-measure.** Re-run `build/visible-fps.mjs` GPU+raster on
   discord.com with ZERO builds running, to remove the load confound and get
   a trustworthy baseline.
4. If spin loop: is Discord retrying on an unsupported API we can stub/throttle
   (WebRTC/MediaEngine, audio asset_404 retries, PostMessageTransport)?

## MEASUREMENT TOOLS (created this session, in build/, gitignored)

- `build/visible-fps.mjs [url] [gpu]` — TRUE visible fps via screenshot-distinct.
  THE tool for this problem. Headed: `DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/visible-fps.mjs https://discord.com 1`
- `build/gpu-visible.mjs` — hello-demo visible-frame sanity (present pipeline health).
- `build/gpu-cadence.mjs` — guest rAF + hostTicks (⚠ engine-side, NOT visible — misled us; keep only as a JS-liveness signal).
- `build/ws-stress-{server,test}.mjs` — WebSocket frame stress (ruled OUT as a cause).
- `build/wasm-oob-test.mjs` + `web/probe/wasmrepro/` — libdiscore reject timing.
- `build/persist-cost.mjs` — persistence dump cadence (ruled out: 60fps held with 4MB mutating profile).

## FIXED THIS SESSION (do not re-investigate — these work)

- libdiscore 59s engine freeze → 10ms reject (705d02f). Confirmed live in user log.
- Intermittent toObjectSlow JSC abort → recovers instead of aborting (d95b601),
  log rate-limited (020df69). User saw clean boots to ChannelMessages.
- wasm bridge null-deref OOB vector → guarded (705d02f). Codex-confirmed.
- W-B2 GPU under pthread + G4 context loss + software-renderer bench guard +
  OffscreenCanvas commit rAF nudge (2b96e64 … babf7fc). gate8 green.

## RULED OUT as the 0.3fps cause

- WebSocket frame handling (stress test clean).
- Persistence dump (throttled to 5s, builds JSON only then; persist-cost held 60fps; user saw no diff persist on/off — pending clean re-confirm).
- The renderer / GPU present pipeline (hello = 60fps).
- My leftover test processes (0 orphaned headless chromium; load was falling).
- GPU vs raster choice (both bad on Discord: 0.3 vs 1.6; the problem is upstream of render mode).

## STANDING STATE

- main @ 020df69. Local only. Servers: dev :8080, wisp :5001 (restart if stale).
- GPU default = !navigator.webdriver (on for humans). ?gpu=0 forces raster.
- Tasks: #57 (toObjectSlow) still open as ROOT-CAUSE (recovery is a mitigation;
  the JSType-reads-0 flake itself is unexplained — likely GC/structure race
  under the pthread). #70 closed. #71 (dirty-rect present) closed — likely
  irrelevant if JS-bound.
