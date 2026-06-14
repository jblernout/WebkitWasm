# Handoff — timer/microtask JS pacing (residual Discord freeze)

> ⚠️ **SUPERSEDED 2026-06-13 ~18:25 EDT — DO NOT ACT ON THIS HANDOFF'S PLAN.**
> This handoff's premise (pace `bib_pump` timer/microtask JS, "pumpGap saturation")
> was FALSIFIED by direct per-cycle + heap instrumentation. `pump` (timer JS) is
> tiny; the freeze is a single synchronous `RunLoop::cycle` up to **10.5s**
> processing Discord's gateway READY at no-JIT speed, at **531MB/4GB (not memory)**.
> The old `pumpGap=700–933ms/s` was a page-side formula mislabeling
> `bib_pump_network` cycles as timer JS. See task #77 (updated) and memory
> `discord-freeze-rootcause-nojit`. A GPT-5.5 consult is pending; this doc will be
> rewritten with the corrected diagnosis + chosen levers afterward.

**Written**: 2026-06-13 ~17:30 EDT. **Active handoff.** Supersedes
`handoff-2026-06-13-gpu-present-stutter.md` (that work is DONE + committed).
Read THIS first; the GPU-present-stutter handoff is now history.

---

## TL;DR — what shipped this session, what's left

**SHIPPED + COMMITTED (main):**
1. **581bc71** — GPU present rewrite (zero-copy bitmap) + **triple-buffer present
   pacing** (one-in-flight → `inFlight<3` + ack-on-receive). Discord GPU present
   delivery 6.6→13.8fps (2.1×). gate8+gate2 PASS, Codex-clean.
2. **1ed013b** — **dynamic rendering-update throttle** (`?rcap`). Caps how often
   WebCore's `updateRendering()` (guest rAF/React JS) runs, derived from an EMA
   of its per-pass cost (≤⅓ of the thread), floating 30–60fps by page cost (no
   hardcoded fps). Light pages 60fps (no penalty), Discord ~28fps cv~0.62 (worst
   stall 1350–2150ms → 250–750ms). gate2+gate8 PASS, Codex-clean. `?rcap=0` off,
   `?rcap=N` fixed. See memory [[rendering-update-throttle]].

**THE REMAINING OPEN PROBLEM (this handoff):** Discord can STILL "freeze for a
few sec" because a second saturation source is **un-addressed**: timer/microtask/
promise guest JS running in **`bib_pump`** (`WTF::RunLoop::cycle()`), measured at
**BIBPERF `pumpGap=700–933ms/s`** on Discord. The render-throttle only caps the
rAF portion (`renderUpd`); `pumpGap` is separate and uncapped. When total guest
JS (rAF + timers + microtasks) maxes the single no-JIT engine thread, paint
starves regardless. **Task #77.**

---

## ROOT-CAUSE MAP (measured, high confidence)

The "smooth then freeze" GPU stutter was THREE things, peeled in order:
1. ~~Present pacing~~ (one-in-flight ack) — FIXED (581bc71), was ~30% of it.
2. ~~rAF/`updateRendering` saturation~~ (`renderUpd` 700–890ms/s) — FIXED
   (1ed013b dynamic throttle).
3. **Timer/microtask saturation** (`pumpGap` 700–933ms/s) — OPEN (#77).

NOT causes (ruled out): the GPU present itself (`present=0–68ms`), pthread (it
HELPED — user-confirmed; raster-under-pthread was smooth; pre-pthread main-thread
GPU was smooth too — the issue was W-B2's cross-thread GPU present, since fixed),
GC (2–6ms), canvas2D acceleration (already on, verified). The freezes are
fundamentally the **no-JIT CLoop running Discord's per-frame JS** on ONE engine
thread; GPU makes them *look* worse than raster only by contrast (fast paint in
the gaps vs raster's uniform crawl).

Per-second profile of a Discord GPU freeze (build/perf-watch.mjs):
```
23.2s tick=113 paint=0 el=1005 | JS=78 rUpd=894 ... pres=0   <<< (pre-throttle: renderUpd ate it)
15.9s tick=60  paint=10 el=1005| JS=37 rUpd=12  ... pumpGap=921 <<< (post-throttle: pumpGap eats it)
```
The throttle moved the bottleneck from `renderUpd` to `pumpGap`. That's #77.

---

## INVESTIGATION PLAN (#77) — pace bib_pump, carefully

**Goal:** leave the engine thread headroom for paint during timer/microtask
bursts, WITHOUT breaking JS semantics (timers/microtasks must still run; no
microtask reordering; don't starve them).

**Leads (in rough priority):**
1. **Interleave a paint into pump cascades.** The event-driven pump fires
   `bib_pump` on every RunLoop wakeup (`setWakeUpCallback`→`bibWakeUp`, main.cpp
   ~1850); a cascade of wakeups = a burst of `bib_pump` with no paint between.
   Cheapest win: ensure `bibPushFrameIfDirty` runs periodically *during* a pump
   burst (e.g., every Nth pump or every ~16ms of pump time), so paint isn't
   starved for the whole burst. LOW risk (doesn't change JS semantics, just adds
   paints).
2. **Coalesce/rate-limit bib_pump bursts.** If many wakeups queue, collapse them
   (like `g_tickQueued` does for bib_tick) so the engine doesn't thrash.
3. **Budgeted RunLoop drain.** Bound work per `bib_pump` then yield. RISKY —
   `WTF::RunLoop::cycle()` may not support partial drains; investigate its
   semantics first. Could break microtask/timer ordering.

**Watch out:** `bib_pump` (via `bib_pump_network`/`hostPump`) also services
curl/network completions — pacing it could add network latency. Check
`bib_pump_network` (main.cpp ~760) vs `bib_pump` (~750) separation.

**Honest ceiling:** pacing only REDISTRIBUTES thread time (steady paint between
JS); it does NOT make Discord's JS faster. Total no-JIT JS work is fixed. The
real JS-speed fix is the wasm-shim path (tasks #54/#55) or accepting the tax.
Manage expectations: #77 should reduce the freeze *duration/contrast*, not
eliminate Discord slowness.

---

## VERIFICATION TOOLS (build/, gitignored)
- `perf-watch.mjs <url> <gpu>` — per-second BIBPERF phase breakdown; flags FREEZE
  seconds + shows `pumpGap` (the #77 quantity). Env: BIB_RCAP, BIB_WATCH.
- `present-cadence.mjs <url> <gpu>` — page-side `__bib.frames` delivered-cadence
  referee (mean fps, cv, stall buckets, cadence strip). Env: BIB_RCAP=0/N,
  BIB_INFLIGHT, BIB_CANVASGPU, BIB_SETTLE, BIB_RUN. (screencast-fps.mjs is DEAD
  under Xvfb — reads ~0 for both modes; don't use it.)
- `web/probe/hardgfx.html?shapes=N&blur=N` — load-tunable present isolator
  (committed). The light default is the "does the throttle hurt light pages" test.
- Gates: `node tools/gate8-gpu-test.mjs`, `node tools/gate2-browser-test.mjs`.
- Build (relink): `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0
  --collect -- env BIB_JOBS=6 bash tools/build-webcore.sh` (embedder/pre-js/cmake
  = incremental relink; flag change = full rebuild). ⚠️ Builds peg 6 cores and
  visibly worsen the engine — never benchmark while a build runs.

## KEY FACTS
- **Discord headless is NOISY** (off swung 11→33fps run-to-run; one dynamic run
  hit 0fps = load fluke, NOT the throttle — paint runs independent of it). Take
  multiple samples; trust the mechanism (BIBPERF phases) over single fps numbers.
- Headless (Playwright fresh profile) Discord = NOT logged in = landing/load page,
  heavier than the user's logged-in steady-state client. The USER's live test is
  the ground truth.
- `?rcap` semantics: absent = dynamic (interactive only; gates excluded), 0 = off,
  N = fixed N/s. Constants: `kRcapUpdateBudget=0.33`, `kRcapMaxIntervalMs=33`
  (30fps floor), `kRcapEmaAlpha=0.2` (main.cpp ~215).
- Working tree: clean except `web/probe/wasmrepro/` (untracked, leave alone).

## TASKS
- #77 (this handoff) — bib_pump timer/microtask pacing.
- #57 (open) — Discord login JSC CLoop abort in toObjectSlow.
- #29/#30 (open epics) — perf + site support.
