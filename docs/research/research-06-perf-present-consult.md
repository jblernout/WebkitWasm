# research-06 — GPT-5.5 Pro perf + present-pipeline consult (2026-06-13)

Verbatim GPT-5.5 Pro response. Brief: build/gpt55-perf-brief.md (10 files attached:
main.cpp, engine-pre.js, browser.html, CLAUDE.md, build-webcore.sh, embedder.cmake,
decision-005, handoff-gpu-present-rearchitecture, research-05, gpt55-present-design).
Conversation URL at the bottom for --continue follow-ups.

VERIFIED BY CLAUDE (code cross-check, 2026-06-13):
- engine-pre.js:157 maxInFlight=3 — CONFIRMED.
- browser.html:170 `port.postMessage({t:"ready"})` BEFORE browser.html:173
  `transferFromImageBitmap` — CONFIRMED (credit-before-composite).
- receiver transfers every frame, no latest-wins stale-drop — CONFIRMED.
  => "latest-wins" is actually bounded FIFO depth-3 + early-ack. GPT-5.5's #1
  finding stands. Non-determinism hypothesis (queue-depth threshold) is plausible
  and falsifiable via the inflight=1/2/3 matrix (knobs already exist).

---

I read the attached files and I agree with your core framing: the remaining conflict is not “GPU present is too slow” in the old readback sense. The zero-copy design is now structurally correct: GPU paints into the persistent texture surface, blits texture to FBO0, flushes, then transfers the OffscreenCanvas ImageBitmap to a bitmaprenderer canvas. The code now matches the intended BIB_PTHREAD × GPU present matrix.  

My verdict: you can improve the smoothness side materially, and probably remove much of the run-to-run variance, but you cannot get “rcap=0 MotionMark-like internal rAF throughput” and “Discord-smooth delivery” at the same time on one WebCore/JSC thread. The only ways to appear to get both are: repeat/stabilize already-produced frames, do approximate host-side scroll/compositor tricks, or run the old main-thread implicit GPU build and accept host-tab freezes. None of those create new WebKit frames while the engine thread is busy inside CLoop/updateRendering.

1. Ranked falsifiable recommendations

2. Fix/measure present backpressure first: current “latest-wins” is not actually latest-wins.

Mechanism: right now browser.html receives every frame message and calls transferFromImageBitmap on each one; engine-pre.js allows up to maxInFlight, default 3, and browser.html returns credit immediately on frame-message receipt, before transferFromImageBitmap. That is bounded FIFO, not latest-wins. It allows up to three GPU-backed ImageBitmaps plus browser compositor work to sit between engine state and visible state. It is also exactly the kind of queue-threshold system that gives “same settings, one run smooth, next run choppy.” 

Change the protocol to one of these two A/B modes:

* Mode A, strict delivery: maxInFlight=1, ack after transferFromImageBitmap, and on worker receipt of ready, schedule an immediate collapsed _bib_tick so one-in-flight does not wait for the next host rAF.
* Mode B, true latest-wins: allow maxInFlight=2 or 3, but main closes/skips stale bitmaps and only transfers the newest id seen in a macrotask/rAF batch. Do not display all queued frames.

Expected payoff: high for cv/stall reduction if the variance is queue/compositor-driven. It may reduce peak MotionMark-visible throughput, but it should make Discord runs repeatable.

Effort: low, mostly engine-pre.js and browser.html.

Risk/tradeoff: strict mode can underfill the pipeline unless you schedule a tick immediately on ack. Latest-wins can drop visually meaningful animation frames, but that is the stated policy and it is better than showing stale frames.

Validate: run present-cadence.mjs with rcap=0, rcap=30, dynamic, and inflight=1/2/3. Add per-frame id/timestamps: worker post time, main receive time, transferFromImageBitmap duration, ack time, inFlight at post, framesSkipped. If bad runs show high id lag, high inFlight, or long receive-to-transfer delays, this is the nondeterminism root. If inflight=1 eliminates the cv 1.65 vs 0.67 split, you have the answer.

2. Replace dynamic rcap with a delivery-first controller, not an updateRendering-only controller.

Mechanism: current dynamic rcap computes interval from EMA(updateRendering cost) / 0.33, capped at 33ms. That only budgets updateRendering. It ignores RunLoop::cycle, pump cycles, net cycles, layout, paint, present, push overhead, ImageBitmap credit state, and host ack latency. The code even records most of those terms in BIBPERF, but the controller does not use them.  

The better controller is delivery-first:

* Pick a visible target interval, initially 33ms for Discord-class pages.
* Track EWMA for updateRendering, layout+paint+present, runloop, pump/net, and present ack latency.
* If a frame is dirty and present is overdue, paint/present before running another updateRendering.
* Only run updateRendering when projected slack is positive: targetInterval - EWMA(layout+paint+present) - safetyMargin > EWMA(updateRendering).
* Compute render update rate from total budget, not update budget alone: allowedUpdateMsPerSec = 1000 - targetFps * frameCost - pumpReserve - safetyReserve.
* If ack latency or inFlight pressure rises, lower update rate immediately.
* If delivered cv stays low and busy < roughly 80%, raise target toward 45/60.

Expected payoff: high. It should preserve rcap=30’s smoothness but adapt better than fixed 30 on lighter pages.

Effort: medium.

Risk/tradeoff: MotionMark score will stay lower than rcap=0 because you are deliberately refusing invisible internal rAF churn. That is the correct tradeoff for a browser UI mode.

Validate: same three metrics every run: MotionMark score, present-cadence fps/cv/stall buckets, and BIBPERF busy breakdown. A successful controller beats rcap=30 on either fps or cv without making worst stall worse. If it cannot beat fixed 30 on Discord, keep fixed 30 as the Discord-class default.

3. Add a “present-before-update when overdue” path.

Mechanism: bib_tick currently runs RunLoop::cycle, then maybe updateRendering, then bibPushFrameIfDirty. That means an already-dirty frame can be held hostage behind another expensive updateRendering pass. The code path is explicit: updateRendering happens before bibPushFrameIfDirty. 

Change tick order conditionally:

* Run RunLoop::cycle.
* If dirty and last visible/posted frame is older than targetInterval, call bibPushFrameIfDirty immediately.
* Then, if budget permits, run updateRendering.
* Then push again only if updateRendering produced new damage and present credit exists.

Expected payoff: medium to high for scroll/input stutter. This does not make the app logically faster; it stops old completed paint work from waiting behind new JS/rAF work.

Effort: low to medium.

Risk/tradeoff: the first frame after input can be slightly stale relative to the newest JS state. For scroll, that is usually better than a 400–1350ms stall.

Validate: compare wheel-to-pixel latency and present-cadence under hard scroll. If latency improves but screenshots show stale/corrected frames, decide whether to gate it only under active input or only when stall age exceeds 50–100ms.

4. Let bib_pump opportunistically present when dirty and overdue.

Mechanism: bib_pump runs RunLoop::cycle only. Under timer/microtask/network bursts, it can consume engine time without any paint until the next rAF-driven bib_tick. The code already tracks pumpCycle/pumpMax specifically to distinguish “one giant cycle” from “many small cycles.” 

Add a guarded path after bib_pump’s cycle: if frame dirty, present credit is available, and last present age > targetInterval, call bibPushFrameIfDirty without updateRendering.

Expected payoff: medium on pages where pump bursts create paint starvation.

Effort: low.

Risk/tradeoff: it can increase paint frequency on pages that generate many small timer changes, so keep it behind the same delivery budget.

Validate: BIBPERF bad run should show pumpCycle/netCycle contributing; after change, pumpMax may remain but stall buckets should drop.

5. Keep BIB_PTHREAD=OFF GPU implicit as a ceiling/reference build, not the default.

Mechanism: the old main-thread GPU path avoids worker-to-main ImageBitmap transfer and recovered the 109.87 @ 144fps MotionMark result. The current design already supports gpu-implicit by leaving #screen untouched and letting the engine own WebGL2 directly.  

Expected payoff: high for MotionMark and for proving the worker/bitmap cost ceiling.

Effort: medium, because it needs a separate BIB_PTHREAD=0 build and validation.

Risk/tradeoff: it reintroduces host-tab freezes during long CLoop tasks. Discord’s 10.8s READY hydration would block the outer page too. That is unacceptable as the daily-driver default.

Validate: build BIB_PTHREAD=0 in a separate dir, run MotionMark and present-cadence. If it returns to ~109 and smooth delivery, the remaining 13% MotionMark regression is mostly threading/bitmap pipeline. If not, the difference is scheduler/timing or later code changes.

6. Increase initial wasm memory for perf runs and A/B malloc.

Mechanism: embedder.cmake uses INITIAL_MEMORY=256MB, ALLOW_MEMORY_GROWTH=1, MAXIMUM_MEMORY=4GB. Your Discord measurement says 531MB used. That guarantees growth during large sessions unless already grown before the critical path. Memory growth and allocator threshold timing are plausible nondeterminism triggers. 

Recommendation: add a perf knob for INITIAL_MEMORY=768MB or 1024MB, and explicitly A/B malloc choices if your Emscripten 6.0 tree supports them cleanly. Keep 256MB for lightweight/default if startup footprint matters.

Expected payoff: low to medium for steady-state, medium for Discord boot variance.

Effort: low, but rebuild/link required.

Risk/tradeoff: higher baseline memory footprint and potentially worse low-memory device behavior.

Validate: compare first 60s Discord BIBPERF, browser memory graph, GC logs, and present-cadence variance across five repeated cold loads.

7. Do the CLoop IC/property fast-path spike, but treat it as boot/hydration work, not present work.

Mechanism: the prior JIT analysis is still right: no practical JIT path, but monomorphic get_by_id/put_by_id/object allocation fast paths are the one plausible in-tree speedup for Discord’s object hydration.  

Expected payoff: probably 1.2–1.8x if a missing CLoop fast path is real; maybe much less if CLoop already hits ICs or Discord is call/dispatch dominated.

Effort: medium to high.

Risk/tradeoff: JSC-internal correctness risk, especially write barriers and structure transitions.

Validate: build a synthetic READY hydration benchmark and add opcode/object-operation counters before changing anything. Continue only if put_by_id/get_by_id/new_object dominate and slow-path rate is high.

8. Add APZ-lite host-side scroll prediction only after the scheduler/present fixes.

Mechanism: on wheel, immediately shift the last displayed bitmap/canvas region on the host, then send the wheel to WebKit and correct when the real frame arrives. This is the only way to improve perceived scroll latency while the engine thread is busy.

Expected payoff: high perceived scroll smoothness on document-like pages.

Effort: medium to high.

Risk/tradeoff: correctness. Fixed/sticky elements, scroll-linked JS, transformed content, and custom scroll containers can smear or jump. This is a UX hack, not a browser-engine correctness path.

Validate: enable only during active wheel, cap prediction duration to ~150ms, disable on pages with obvious fixed/sticky damage if detectable, and compare interact-latency distinct changes plus visual captures.

9. Do not prioritize a dedicated present/compositor worker yet.

Mechanism: a separate presenter worker can own a transferred OffscreenCanvas and consume ImageBitmaps off the main thread, but it still cannot produce new WebKit frames. It only helps if the browser main thread’s port handler or bitmaprenderer call is the measured bottleneck.

Expected payoff: low unless instrumentation shows main receive/transfer is consistently slow.

Effort: medium.

Risk/tradeoff: more cross-thread plumbing and more failure modes around canvas ownership.

Validate: only pursue if frame timestamps show main receive-to-transfer or rAF/main backlog is a significant part of bad runs.

10. LTO/PGO are worthwhile later, not first.

Mechanism: build-webcore.sh uses Release, -msimd128, pthread when enabled, JIT off, CLoop on, system malloc on. I do not see LTO/PGO in the build script. 

Expected payoff: single-digit to low-teens percent for interpreter/runtime-heavy code, maybe useful but unlikely to fix cv/stalls.

Effort: medium to high because WebKit+wasm link cycles are expensive.

Risk/tradeoff: build instability, much longer link, harder debugging.

Validate: compare BIBPERF runloop/updateRendering and synthetic hydration, not just MotionMark.

2. Bugs / code-path issues

main.cpp -> dynamic rcap can be worse than fixed 30 -> controller budgets only updateRendering, while BIBPERF shows the thread has runloop, pump, net, layout, paint, present, pushOther, and persist terms. Fix by using total delivery budget and present credit/ack latency, not only updateRendering EMA.  

main.cpp -> present starvation behind updateRendering -> bib_tick runs updateRendering before bibPushFrameIfDirty. Fix with present-before-update when dirty and overdue. 

main.cpp -> pump can starve paint -> bib_pump and bib_pump_network run RunLoop::cycle without paint. Fix with a guarded opportunistic present path after pump cycles when dirty and overdue. 

engine-pre.js / browser.html -> “latest-wins” is not implemented -> default maxInFlight=3 plus FIFO presentation means intermediate frames are queued, not dropped. Fix with strict one-in-flight plus immediate tick-on-ack, or true latest-wins dropping/closing stale bitmaps. 

browser.html -> credit is returned before transferFromImageBitmap -> the worker can produce more frames before the current frame is actually transferred to the canvas. Fix by acking after transfer in strict mode; keep early ack only as an explicit throughput experiment. 

main.cpp comments -> one-in-flight comment is stale/misleading -> C++ says “one-in-flight” but JS implements maxInFlight=3. Fix comments and logs so future debugging does not assume the wrong invariant. 

browser.html -> __bib.frames is “submitted to canvas,” not strictly “user perceived” -> it increments after transferFromImageBitmap returns, before the browser’s next composite/scanout. Fix by splitting framesSubmitted and framesDisplayed; increment displayed in the next requestAnimationFrame after transfer. Keep old counter only if you define it as “canvas updated.” 

embedder.cmake -> 256MB initial memory is probably too low for Discord perf mode -> growth is enabled up to 4GB, but Discord already exceeds 512MB. Fix with a perf/profile initial-memory knob. 

BIB_PTHREAD=OFF path -> implemented but not verified -> the handoff explicitly says it still needs a separate build and MotionMark confirmation. Fix by making it a supported CI/perf target, not just dead code. 

3. The non-determinism

Highest-conviction root cause: queue dynamics at the GPU bitmap/compositor boundary, with GC/memory growth as likely trigger noise. The bad pattern is: rcap=0 saturates the engine; tiny timing differences determine whether the ImageBitmap/compositor queue stays shallow or crosses a backlog threshold. Once crossed, visible delivery becomes bursty even if internal rAF/update throughput is high.

The fastest confirmation is not more speculation; it is this matrix:

* rcap=0, inflight=1, ack-after-transfer, tick-on-ack.
* rcap=0, inflight=2.
* rcap=0, inflight=3 current behavior.
* rcap=30 with the same three inflight modes.
* gpu=0 raster as a control.
* gclog=1 and initial-memory=1GB as separate controls.

If inflight=1/2 collapses the cv spread while raster is stable, it is present queueing. If gpu=0 still has the spread and gclog/memory-growth correlates, it is JSC/GC/allocator. If neither changes it, inspect pumpMax/netMax and long RunLoop::cycle bursts.

4. Bound on “high score and smooth”

The hard bound is simple: updateRendering runs guest rAF callbacks and the JS/style/layout they dirty on the same engine thread that must later paint and present. MotionMark rewards internal guest rAF throughput. The user sees delivered frames. When updateRendering consumes the thread, the score can rise while delivery becomes irregular. The current architecture can decouple transfer from the browser main thread, but it cannot execute JSC rAF work and WebCore paint/present concurrently on one WebCore/JSC thread. 

So the real target should be two modes:

* Benchmark/ceiling mode: BIB_PTHREAD=OFF or rcap=0, maximize MotionMark, accept host/visible risk.
* Daily-driver mode: BIB_PTHREAD=ON, GPU bitmap, delivery-first scheduler, Discord-class default around 30 visible fps, minimize cv and worst stalls.

Given your own measurements, the cheapest near-optimal operating point today is rcap=30, GPU bitmap, then A/B inflight 1/2/3. Fixed rcap=30 already beats dynamic on Discord: higher delivered fps, lower cv, fewer stalls. Dynamic should not remain the default for heavy SPAs until it uses total contention and present feedback.

5. Single highest-conviction “do this first”

Instrument and fix the present credit semantics before touching WebKit/JSC again.

Specifically: add frame id/timestamp/inFlight/ack-latency logging, run the inflight=1/2/3 matrix, then switch the default to either strict one-in-flight with tick-on-ack or true latest-wins. This is the shortest path to explaining the cv 1.65 vs 0.67 nondeterminism, and it directly attacks the delivery side without sacrificing the already-correct zero-copy GPU architecture.


CONVERSATION_URL: https://chatgpt.com/c/6a2e1ab2-207c-83ea-ba6b-f13ef98c9424
