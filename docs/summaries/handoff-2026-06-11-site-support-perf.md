# Handoff — Phase 4 usability: site support + perf (the ONE active handoff)

**Written**: 2026-06-11 ~01:45 EDT, after the dirty-rect + Workers W-A
session. **Supersedes**: handoff-2026-06-10-phase5-chrome.md (→
docs/archive/ — read only for deep history of the 2026-06-10 sessions).
**Today's commits**: e48bd55 (sweep tooling) → e0dd124 (wave-2 verdicts) →
e23926e (decision-004 Workers scoping) → 0203316 (dirty-rect #37) →
a5c991c (Workers W-A) → 0b1e1d1 (handoff ptr). Rollback tag for the two
implementation commits: **phase4-pre-dirtyrect-workers**.

## Honest status

**The engine browses the real web and now feels like it.** TLS in-engine,
cookies, storage, IDB, images, iframes, rAF, web fonts (woff2), dedicated
workers (main-thread mode), event-driven pump, dirty-rect rendering.
Wave-2 sweep: old.reddit real front page, google login full flow ≤3s,
reCAPTCHA 0.42s click-feel + interactive 4×4 image challenge, velzie.rip
near-perfect, fal.ai fine. Perf numbers on record: MessageChannel hop
0.38ms, setTimeout 6.93ms, fetch 44ms, **dirty frame 0.35ms (was
32.09ms)**, idle bib_tick 0.005ms. The TWO repeated engine gaps from the
sweep are now ONE: ~~Workers~~ (W-A live) and **WebGL (#32)**.

## ⟶ NEXT SESSION STARTS HERE: blit-shift guard fix, then Skia-GPU G1

**RUNG 1 VALIDATED 2026-06-11 ~12:30 (task #42 complete) — honest mixed
verdict; build is HEALTHY and stays:**
- Gate pixel-exact under SIMD; anim probe 0.34ms; reddit renders clean.
- **-msimd128 ≈ ZERO paint win on real content**: reddit full repaint
  **32.18ms vs 32.09ms scalar**. Text-dominated CPU raster doesn't
  autovectorize; if SIMD is to matter, Skia's dedicated wasm SIMD opts
  need explicit investigation — but the better conclusion: **GPU
  (decision-005) is THE paint lever, full stop.**
- **Blit-shift DORMANT**: scroll probe = 40/40 full-viewport boxes @
  24.09ms. The g_frameDirty stale-pixel guard in bibScrollBlit trips on
  EVERY scroll tick (WebCore invalidates scrollbars/content BEFORE
  calling ChromeClient::scroll). FIX FIRST (small): translate the pending
  g_dirtyRect by the scroll delta (damage moves with content) instead of
  bailing; re-run build/scroll-cost.mjs expecting thin strips.
- **NEW TRAPS** (also in task #42): (a) playwright `browser.close()`
  hangs FOREVER on the COOP/COEP engine page — found two zombie probe
  scopes (11h56m, 1h38m); add close-timeout armor (Promise.race +
  process.exit) to all probe scripts; check for stale run-p*.scope units.
  (b) Full-rebuild reclaim-livelock: nproc-parallel unified-sources TUs
  at -O3 -msimd128 need ~1.2GB EACH → 12G/no-swap scope livelocks (28min
  wall/3min CPU per job, counter frozen at [1676/2089] for 30+ min).
  `BIB_JOBS=6` knob now in build-webcore.sh — USE IT for full rebuilds.
- Post-SIMD 5-site sweep NOT yet run (only reddit via probes) — run as
  part of the next session's regression pass.

Then: **Skia-GPU G1 context spike** (decision-005) — now unambiguously
the top perf item since SIMD underdelivered on paint.

## (superseded by the block above) Previous opener: validate rung 1

**2026-06-11 ~02:55 update — acceleration work started after this handoff
was first written; the WebGL-spike opener below is now SECOND in line.**

State at compaction: commit **cda7af6** = scroll blit-shift + `-msimd128`
(PENDING VALIDATION); decision-005 (commit 384f9f8) = Skia-GPU scope
(headline: SK_GL+SK_GANESH and the full Ganesh GL backend are ALREADY
compiled into our libSkia.a; WebCore Skia layer is GPU-aware;
integration = PlatformDisplay::skiaGrContext provider + WebGL2 context +
RenderingMode::Accelerated; ~1.5–2.5wk; G1 context spike first).
**A FULL `-msimd128` REBUILD WAS IN FLIGHT at session end** (~878/2089
when last checked; every object in the tree recompiles — engine was 100%
scalar wasm before this).

Validation steps (run in order once the build lands; run
`bash tools/build-webcore.sh` once more first — incremental — to be sure
the final main.cpp edits compiled):
1. Node gate (`tools/run-embedder.cjs` under 8G scope) — pixel-exactness
   under SIMD blitters; expect exactBlue=20000, redGlyph=1962 unchanged.
2. `build/dirty-cost.mjs` (anim probe) — 0.35ms scalar baseline.
3. `build/paint-cost.mjs` (old.reddit) — **32.09ms scalar baseline;
   expect ~12–20ms with SIMD**.
4. `build/scroll-cost.mjs` (NEW, on web/probe/tall.html — plain page so
   canBlitOnScroll holds) — expect thin-strip dirty boxes + few
   full-viewport frames; also eyeball old.reddit scroll.
5. 5-site sweep spot check (site-diagnose, BIB_CHANNEL=chromium).
6. Mark #42 complete; amend/extend cda7af6's message or commit results.
Codex review of the blit-shift ALREADY DONE (2 mediums fixed in cda7af6:
g_inPaint guard, canvas-writePixels failure resync). Reality check
already documented: blit-shift only fires on pages without fixed/sticky
elements (GitHub takes the slow path — SIMD/GPU are its cure).

Then: **Skia-GPU G1 context spike** (decision-005) or the WebGL spike
below — note decision-005 OQ5: doing GPU-G1/G2 first likely makes guest
WebGL (#32) much cheaper (shared context, zero-copy composite), so
consider folding #32's route decision into G1.

## (now second in line) Previous opener: WebGL spike (#32)

WebGL is the last repeated engine gap on real sites (velzie.rip
background `gl.viewport null`, happy_wheels PixiJS needs
WebGLRenderingContext; plenty of the wider web wants WebGL1). The spike
is a **1-day route decision, NO implementation commitment**:

1. Route (a): vendored ANGLE (Source/ThirdParty/ANGLE) compiled to wasm —
   weeks, huge build surface, EGL platform glue unknowns. Route (b):
   custom `GraphicsContextGLEmscripten` implementing WebKit's
   GraphicsContextGL interface directly on Emscripten's GLES2→host-WebGL
   translation, composited into the Skia CPU frame via readPixels —
   est. 1–2 weeks for a WebGL1 subset. Every existing concrete backend is
   ANGLE-based (GraphicsContextGLANGLE; TextureMapper variants wrap it) —
   route (b) means writing a new one against the cocoa/ANGLE impl as
   reference.
2. Spike deliverables: does ENABLE_WEBGL=ON compile without dragging
   ANGLE in (option/cmake surface)? What does WebGLRenderingContext
   creation need from the page client (GraphicsClient::createGraphicsContextGL)?
   Inventory the GCGL interface size actually exercised by WebGL1 +
   velzie's shader + PixiJS. Emscripten context creation constraints
   (-sOFFSCREEN_FRAMEBUFFER, context on the host canvas vs offscreen).
   Output: decision-005-webgl-route.md, go/no-go + phasing.
3. Cheap alternative opener if appetite is low: option B below (sweeps).

## The menu — every option on the table, ranked

- **A. WebGL spike (#32) ← RECOMMENDED OPENER** (1 day, decision only;
  unlocks the #1 remaining engine gap if pursued).
- **B. Wave-3 site sweeps** (cheap, hours): re-verdict with workers +
  dirty-rect live; add NEW sites (wikipedia, github, x/twitter login
  wall, discord login, a webshop, news sites). Each sweep historically
  re-ranks the menu — and with workers live, sites that fell back may
  take new code paths. Watch for: worker NetworkError/NotSupportedError
  lines (= sites hitting the W-A gates → evidence for W-B triggers).
- **C. Phase 5 chrome trio** (small, filler-grade, independent):
  title/URL/progress callbacks (BibFrameLoaderClient dispatch* overrides
  → EM_ASM host events), back/forward (real in-memory BackForwardList —
  EmptyBackForwardClient is capacity 0), bib_stop/bib_reload. Pure
  usability; user framed chrome as secondary to site usability.
- **D. Page-level WebSocket (CurlStream multi-interface rework)** —
  medium (~150–250 lines in CurlStream/CurlStreamScheduler, reuse the
  hostPump pattern). **Sequencing rule: do NOT build this if W-B is
  plausible** — under real pthreads the upstream blocking design works
  as-is (network thread blocks, main services SOCKFS) and this comes
  nearly free.
- **E. Workers Phase W-B (real pthreads) — HOLD.** Re-open ONLY when a
  trigger fires: a target site's worker needs importScripts / module
  workers / WebSocket-in-worker / real parallelism (sweep evidence from
  option B is the detector). Cost 1–2 weeks. Pre-flip MUSTs are already
  scoped in decision-004: dispatchSync wrong-thread fix, cross-thread
  main-RunLoop wake (MAIN_THREAD_ASYNC_EM_ASM), separate build dir
  build/webcore-mt behind BIB_THREADS=1 (sysroot is ALREADY pthread-ABI —
  verified; only WebKit tree + embedder recompile). Bundles: real worker
  threads + async image decode back (ImageFrameWorkQueue revert) +
  page-WS nearly free. Tax: ~5–15% scalar perf (measure before default).
- **F. Guest WebAssembly epic — PARKED.** The ACTUAL gate for
  Turnstile + Discord (ENABLE_WEBASSEMBLY hard OFF under CLoop; JSC wasm
  needs JIT-tier or IPInt machinery we don't have). Big, unscoped.
  Revisit only after WebGL settles.
- **G. Small/cosmetic backlog**: bundle WebCore's missingImage broken-icon
  (cosmetic warning on every image-404 page); cookie persistence to OPFS
  (in-memory only today — logins don't survive reload); HTTP auth
  401/407 (error-stubs); fal.ai hero-canvas purple-noise artifact
  (minor, unexplored); optional upstream report for the headless-shell V8
  SEGV (#38 — BIB_CHANNEL=chromium mitigation in place).
- **CLOSED, don't re-open**: bib_tick profiling (idle 0.005ms — pump is
  not a cost center); dirty-rect #37; Workers scoping #40 + W-A #41;
  woff2 (works, was a bad test asset); happy_wheels abort (host V8 bug,
  mitigated); curl=35/92 (environmental: LAN ad-block DNS + rinici.de
  bot filter).

## What shipped 2026-06-11 (this session)

- **Wave-2 sweep verdicts** (task #39): every site improved, zero
  aborts/crashes; details in recap-2026-06-10-2350.md.
- **decision-004-workers-scope.md** (task #40): Workers scoping, the
  UseMainThread discovery, sysroot-already-pthread-ABI verification,
  W-B ledger re-audit inventory + risks. Codex-cross-checked.
- **Dirty-rect #37** (commit 0203316 + fixes in a5c991c): damage union in
  BibChromeClient (rects were always delivered, previously collapsed to a
  bool), snapshot-and-clear BEFORE paint (Codex: paint-time damage must
  survive to next frame), clip-paint + partial readPixels + partial
  putImageData (bib_dirty_box export). **32.09ms → 0.35ms** per dirty
  frame; box pixel-exact; node gate pixel-identical; old.reddit clean.
  Probes: web/probe/anim.html + build/dirty-cost.mjs (gitignored).
- **Workers W-A** (commit a5c991c, task #41): main-thread dedicated
  workers via WorkerThreadMode::UseMainThread. Six WebKit hunks — see
  decision-004 "W-A implementation results" for the full list and the
  upstream find (WorkerMainRunLoop::postTaskForMode silently DROPS
  pre-bootstrap tasks; proxy flushes early postMessages before
  thread->start() enqueues setGlobalScope → port requeue fix). Fail-fast
  gates (all catchable): module workers NotSupportedError, in-worker
  WebSocket NotSupportedError, importScripts/sync-XHR NetworkError,
  crypto wrap/unwrap nullopt. Smoke 6/6 (web/probe/worker.html via
  build/worker-smoke.mjs); reCAPTCHA zero worker errors + interactive
  challenge; 5-site sweep at baseline (build/sweep3-wa.log).

## State / how to run

- **Stack** (three processes):
  `node tools/dev-server.mjs web --mount /engine=build/webcore/bin` ·
  `npm run wisp` (5001; loopback allowed = DEV ONLY, SSRF hole if public) ·
  open `http://127.0.0.1:8080/browser.html?url=https://...`
- **Builds**: ALL under `systemd-run --user --scope -p MemoryMax=12G
  -p MemorySwapMax=0 --collect --`; browser/Playwright runs under 8G.
  `bash tools/build-webcore.sh` is resumable/incremental. NEVER
  `rm -rf build/webcore`.
- **Heavy-JS site tests**: `BIB_CHANNEL=chromium` (headless-shell V8
  SEGV mitigation #38; knob in site-diagnose.mjs + google-login-repro.mjs).
- **Gates**: tools/run-embedder.cjs (pixel-exact: exactBlue=20000,
  redGlyph=1962), gate2–gate7, urlbar-test, smoke-modern-site.mjs;
  site-diagnose.mjs for sweeps; google-login-repro.mjs 600 134;
  build/recaptcha-click.mjs (gitignored).
- **Patch ledger**: src/patches/webkit-emscripten.patch, 2738 lines,
  60 files — tree == ledger (export verified). Every WebKit-tree edit →
  `tools/export-webkit-patches.sh` BEFORE commit.
- embedder.wasm ~106MB; binary at build/webcore/bin/.

## Known traps (carried forward + new)

- Empty-client semantics bite SILENTLY — check what
  pageConfigurationWithEmptyClients installed before suspecting wasm
  (TEN+ root causes were this family).
- WEBKIT_OPTION_DEFAULT_PORT_VALUE only affects FRESH CMake caches —
  existing build dirs need -D<OPT>=… once.
- Wide ninja rebuilds can OOM-kill em++ silently — just re-run.
- bash `have=$(rg …)` under set-e/pipefail dies on no-match — `|| true`.
- WTFLogAlways probes beat static tracing; REVERT before patch export.
- **NEW**: only `Module.HEAPU8` is an exported heap view — build other
  typed views from `Module.HEAPU8.buffer` (Module.HEAP32 is undefined;
  cost one dead blitLoop to learn).
- **NEW**: `bib_render(0)` only updates the dirty region of the buffer —
  pixel probes that sample ARBITRARY coordinates must use
  `bib_render(1)` (force = full frame, also resets bib_dirty_box to full).
- **NEW**: worker gates are catchable by design — a site logging
  NotSupportedError (module worker / in-worker WS) or NetworkError
  (importScripts) is hitting the W-A limits; collect as W-B trigger
  evidence, don't treat as a regression.
- foreground `sleep` is blocked in the harness — background until-loops.
- Local git only. Codex review before presenting non-trivial code.
