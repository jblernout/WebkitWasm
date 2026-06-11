# Decision 004 — Web Workers: scope, cost, and phasing (task #40)

Date: 2026-06-11. Status: **PROPOSED** (scoping deliverable; no build flip
performed). Evidence base: wave-2 sweep (task #39) — Workers is the only
engine-gap error repeated across all five real sites
(`NotSupportedError: Web Workers are not supported in this single-threaded
engine`, fired by 2captcha/reCAPTCHA on load AND +2.3s post-click).

## Verdict up front

- **GO — Phase W-A: main-thread workers.** No `-pthread` flip, no ledger
  re-audit, no dep work. Upstream WebKit already ships a
  run-workers-on-the-main-thread mode; enabling it for dedicated workers is
  a ~3-hunk patch. Cost ≈ **1–2 days** incl. testing. Confidence: high
  that it builds and runs; medium that it satisfies reCAPTCHA (empirical
  question W-A exists to answer).
- **HOLD — Phase W-B: real pthread workers.** Cheaper than feared (the
  sysroot is ALREADY pthread-ABI — verified below), but it needs a full
  WebKit-tree recompile, one mandatory correctness fix (`dispatchSync`),
  one new mechanism (cross-thread main-RunLoop wake), and a perf
  re-baseline. Cost ≈ **1–2 weeks**. Only start if W-A's empirical results
  demand it (worker needs `importScripts`/sync-XHR or real parallelism).

## The discovery that reshapes the epic: WorkerThreadMode::UseMainThread

Upstream WebKit can run a worker WITHOUT an OS thread, cooperatively on the
main thread. The plumbing is complete and live (service workers use it via
`shouldRunServiceWorkersOnMainThreadForTesting()` and service-worker pages):

- `WorkerOrWorkletThread.cpp:51` — `constructRunLoop()` builds a
  `WorkerMainRunLoop` instead of a `WorkerDedicatedRunLoop` when mode is
  `UseMainThread`.
- `WorkerThread.cpp:123` — `createThread()` with a `WorkerMainRunLoop`
  dispatches the worker bootstrap to `RunLoop::mainSingleton()` and returns
  `Thread::currentSingleton()`. **No `Thread::create`, no pthread.**
- `WorkerGlobalScope.cpp:112` — a worker scope constructed on the main
  thread shares `commonVM()`: **zero per-worker VM cost**, and the
  one-mutator-per-VM GC invariant is untouched.
- `WorkerOrWorkletThread::start()` is fully async (no semaphore handshake
  with the main thread) — nothing in the start path needs a second thread.

What selects the mode: `WorkerMessagingProxy.cpp:162` hardcodes
`WorkerThreadMode::CreateNewThread` for dedicated workers. That line is the
switch.

### Phase W-A patch (3 hunks + stubs)

1. `WorkerMessagingProxy.cpp:162`: `CreateNewThread` →
   `UseMainThread` under `#if defined(__EMSCRIPTEN__)`.
2. Remove the `Worker::create` throw (ledger hunk at
   `Source/WebCore/workers/Worker.cpp` — currently returns
   `NotSupportedError` before any worker machinery runs).
3. Fail-fast stubs for the two synchronous-load paths that would HANG this
   mode (see "hang mechanics" below): `WorkerGlobalScope::importScripts`
   (`WorkerGlobalScope.cpp:434` → `loadSynchronously`) and sync XHR in
   workers — both ride `WorkerThreadableLoader::loadResourceSynchronously`
   (confirmed: its wait loop at `WorkerThreadableLoader.cpp:82` spins
   `runInMode` until done); stub them to throw `NetworkError` under this
   port (sync XHR is already documented-unsupported port-wide).
4. Fail-fast the two `BinarySemaphore` post-then-wait sites in
   `WorkerGlobalScope` (Codex cross-check finding):
   `serializeAndWrapCryptoKey` (`WorkerGlobalScope.cpp:519`) and
   `unwrapCryptoKey` (`WorkerGlobalScope.cpp:536`) — both post a task to
   the main loop then block on a semaphore, a guaranteed same-thread
   deadlock under this mode (structured-clone/IDB-store of a CryptoKey
   from a worker). **Currently unreachable** — this port's WebCrypto is
   link-stubbed (`CryptoStubsEmscripten.cpp`: no algorithms register, so
   no CryptoKey can ever be constructed) — but it becomes a live deadlock
   the day the real OpenSSL WebCrypto backend lands. Stub to fail now
   (return empty/false), it's two lines. Same pattern exists in
   `WorkerNotificationClient::checkPermission` but `ENABLE_NOTIFICATIONS`
   is hard OFF — note for whenever notifications are enabled.

**Hang mechanics (why the stubs are mandatory):**
`WorkerMainRunLoop::runInMode` (`WorkerRunLoop.cpp:478`) is
`RunLoop::mainSingleton().cycle()` in a loop. A synchronous load pumps that
loop until the response arrives — but wisp bytes arrive as host WebSocket
`onmessage` JS events, and JS never runs while wasm spins `cycle()`. The
load can never complete: infinite spin, engine hang. Same root cause as the
existing sync-XHR/CurlStream limitation. Catchable `NetworkError` keeps
pages alive instead.

### W-A limitations (accepted for the phase)

- No parallelism: worker JS interleaves with page JS on the main RunLoop.
  CPU-heavy workers jank the page (CLoop is slow already; acceptable).
- A `while(1)` worker freezes the engine (no preemption). JSC watchdog per
  worker scope is a possible later mitigation; out of W-A scope.
- Classic workers that call `importScripts` get `NetworkError` — whether
  reCAPTCHA's worker does this is exactly what the prototype measures.
  Module workers (`type:"module"`) use async imports and are unaffected.
- Untested-upstream combination (dedicated worker + UseMainThread is built
  but only exercised for service workers): expect 1-2 small landmines in
  `WorkerMessagingProxy` (e.g. termination/GC interplay). Risk: medium,
  cheap to discover.
- Guest `SharedArrayBuffer`/`Atomics.wait` between page and same-thread
  worker would deadlock — audit JSC option default
  (`useSharedArrayBuffer`) and force it off for this mode.

## Phase W-B: real pthread workers — verified build facts

**The sysroot is already pthread-ABI.** Verified 2026-06-11 by dumping the
wasm `target_features` section of installed archives
(`llvm-objdump -s --section=target_features`): `libsqlite3.a`,
`libcurl.a`, `libicuuc.a`, `libfreetype.a` all carry `atomics` +
`bulk-memory`. All three dep scripts compile `-O2 -pthread`
(`webcore-deps.sh:15`, `curl-tier.sh:16`, `icu.sh:50` — "pthread ABI must
match the engine"); OpenSSL is configured with `threads`, SQLite with
`SQLITE_THREADSAFE=1`. Only curl's *threaded resolver* is disabled
(`--disable-threaded-resolver`) — irrelevant, DNS happens server-side over
wisp.

⇒ The note in `src/embedder/embedder.cmake:34` ("the entire library stack
(WTF/JSC/WebCore/sysroot) is compiled single-threaded") is **stale/wrong
about the sysroot**. Only the WebKit build tree + embedder are
non-atomics.

**Flip cost, build side:**

- Recompile the WebKit tree (WTF/JSC/WebCore/PAL + vendored Skia) and the
  embedder with `-pthread`. That is the full ~600-target build (hours,
  under the 12G systemd scope) — but **zero sysroot work**.
- Use a SEPARATE build dir `build/webcore-mt` behind a `BIB_THREADS=1`
  knob in `tools/build-webcore.sh`. `build/webcore` stays untouched (hard
  rule: never delete it; also keeps an A/B perf reference). Disk cost: a
  second ~full build tree.
- Link flags (embedder.cmake, MT branch): `-pthread`,
  `-sPTHREAD_POOL_SIZE=4`, `-sPTHREAD_POOL_SIZE_STRICT=0` (on-demand
  growth works because the event-driven pump (#17) yields the main thread
  constantly), `-sDEFAULT_PTHREAD_STACK_SIZE=2MB` (precedent: the Phase 1
  jsc-shell hunk already carries this flag, currently inert).
  Keep `-sINITIAL_MEMORY=256MB -sALLOW_MEMORY_GROWTH=1`.
- `-sMAXIMUM_MEMORY=4GB` + shared memory means the SharedArrayBuffer is
  reserved with a 4GB max at instantiation. Chromium handles this; if
  allocation fails on a host, fall back to 2GB for the MT build.
- Host page: COOP/COEP already served (dev server, day one) — SAB is
  available. New asset: pthread builds emit a worker bootstrap; serve it
  same-origin (COEP-safe) next to embedder.js.
- Memory growth + pthreads is supported but makes JS-side heap views
  refresh on growth — our per-frame `HEAPU8` blit reads must re-fetch the
  view after growth events (they already go through `Module.HEAPU8`, which
  Emscripten refreshes — verify at prototype).

## W-B ledger re-audit inventory (every single-threaded surgery, with action)

| # | Surgery (ledger hunk) | Under real threads | Action |
|---|---|---|---|
| 1 | `WorkQueueGeneric.cpp` `platformInitialize` — EVERY WorkQueue aliases the main RunLoop | Still correct (queues are serial, run on main). Workers don't need real queues | **KEEP** |
| 2 | `WorkQueue.cpp` `dispatchSync` — runs function **inline on the caller** | **WRONG-THREAD BUG**: a worker calling `dispatchSync` on a main-aliased queue would run the task on the worker thread | **FIX REQUIRED before flip**: inline only when `isMainThread()`; otherwise real semaphore dispatch (futex wait is legal off-main) |
| 3 | `WorkQueue.cpp` `ConcurrentWorkQueue::apply` — serial on caller | Correct, just not parallel | **KEEP** (optional revert later for perf) |
| 4 | `RunLoopGeneric.cpp` `runImpl` — `EM_ASM bibArmTimer` arming | Guarded by `if (Module.bibArmTimer)`, undefined on pthread JS contexts — safe. **BUT**: `callOnMainThread`/`RunLoop::wakeUp` from a worker has NO host-pump wake → main-bound tasks sit until the next unrelated wake | **NEW WORK REQUIRED**: off-main `wakeUp()` must wake the host pump (e.g. `MAIN_THREAD_ASYNC_EM_ASM` or `emscripten_dispatch_to_thread`) |
| 5 | `ThreadingPOSIX.cpp` — `Thread::suspend` RELEASE_ASSERT, sigsuspend removed | Invariant HOLDS: worker VMs are created on the worker thread (`WorkerGlobalScope.cpp:112` → `VM::create`), one mutator per VM, GC never suspends foreign threads. `ENABLE_SAMPLING_PROFILER`/`REMOTE_INSPECTOR` stay OFF (hard constraint) | **KEEP** |
| 6 | `IDBBackingStore.h` — main-thread guard relaxed | IDB server stays on main (aliased queue); workers reach it via `IDBConnectionProxy`, which is cross-thread by design | **KEEP, re-audit** |
| 7 | `IDBBindingUtilities.cpp` — `callOnIDBSerializationThreadAndWait` inline + dedicated VM on main | Called server-side (main) only | **KEEP, re-audit** |
| 8 | `AsyncFileStream.cpp` — `callOnFileThread` → deferred main task | Worker blob reads proxy to main; deferred-main still correct. `FileReaderSync` in workers: in-memory blobs likely fine, audit | **KEEP, re-audit** |
| 9 | `ImageFrameWorkQueue.cpp` — sync decode via `callOnMainThread` | Main-only path (no OffscreenCanvas) | **KEEP** |
| 10 | `CurlRequestScheduler`/`CurlStreamScheduler`/`CurlContext` — network on main RunLoop | By-design even on threaded ports (network on one thread); worker loads proxy via `WorkerThreadableLoader` | **KEEP** |
| 11 | `Worker.cpp` — constructor throws NotSupportedError | The gate itself | **REMOVE** (behind setting/flag) |
| 12 | `ScrollAnimator`, CookieJarDB single-thread use, `EditorEmscripten`, a11y stubs | Main-thread-only subsystems, workers never touch them | **KEEP** |

Also on the audit list: JSC under CLoop+threads is upstream-supported
(JSCOnly runs test262 agents on real threads with CLoop), `USE_SYSTEM_MALLOC`
(dlmalloc) takes a global lock under `-pthread` (correct, slightly slower),
and main-thread futex waits busy-spin under Emscripten (keep main-side
critical sections short — they already are).

## Per-worker memory cost (wasm32 4GB ceiling)

- **W-A: ~0.** Shares `commonVM()` and the main thread.
- **W-B:** `JSC::VM::create(HeapType::Medium)` + CLoop stack + GC blocks
  ≈ low single-digit MB baseline, + **2MB** pthread C stack, + script heap.
  Budget ≈ **5–10MB/worker** before script data. With the engine's
  steady-state ~500MB–1GB on heavy sites, dozens of workers fit; recommend
  a **port-side cap of 8 concurrent workers** (sites like Discord spawn
  worker pools sized to `hardwareConcurrency` — also consider clamping
  `navigator.hardwareConcurrency` to 2–4).
- Each pooled pthread instantiates the (~106MB) module — compiled code is
  shared by the host, instantiation is tens of ms; pool of 4 keeps startup
  invisible.

## Per-site unlock truth table

| Site | Blocking gap today | W-A unlocks? | W-B unlocks? |
|---|---|---|---|
| 2captcha/reCAPTCHA v2 | Worker wanted on load + post-click (suspected fallback-variant trigger) | **Likely** — IF its worker avoids `importScripts`/sync-XHR (empirical question the prototype answers) | Yes (modulo guest-wasm use inside the worker) |
| Cloudflare Turnstile | **Guest WebAssembly** (`ENABLE_WEBASSEMBLY` hard OFF under CLoop) | No | No — wasm is the binding gate, workers secondary |
| Discord | Guest WebAssembly + heavy SPA | No | No — same |
| velzie.rip background, happy_wheels | WebGL (#32) | No | No — unrelated |
| Generic SPAs (Sentry, Monaco/CodeMirror, pdf.js, image-decode pools) | Workers feature-tested, fallbacks already run via catchable throw | Incremental (quieter consoles, real worker paths) | Incremental + real parallelism |

Honest summary: **reCAPTCHA is the only sweep-verified site where Workers
is the binding gap.** Turnstile/Discord stay gated on guest wasm either
way. That asymmetry is why W-A-first is the right shape: it answers the
reCAPTCHA question for ~5% of W-B's cost.

## Risk register (ranked)

1. (W-B) `dispatchSync` wrong-thread inline — correctness, MUST fix
   pre-flip. **High** if missed, trivial to fix.
2. (W-B) No cross-thread main-RunLoop wake — worker→main tasks stall until
   an unrelated wake; everything "works" but goes molasses. **High**,
   needs the new wake mechanism.
3. (W-A) reCAPTCHA worker uses `importScripts` → still falls back →
   W-A doesn't pay off for the headline site. **Medium** — this is the
   go/no-go datum for W-B.
4. (W-A) Untested upstream combo (dedicated worker + UseMainThread)
   landmines in `WorkerMessagingProxy`. **Medium**, cheap to surface.
5. (W-B) ST throughput regression from atomics + locked malloc across the
   whole engine (CLoop is the hot loop). Re-run perf baselines
   (MessageChannel hop 0.38ms / setTimeout 6.93ms / fetch 44ms) on the MT
   build; if regression >10%, MT does not become the default ship.
   **Medium.**
6. (Both) `while(1)`/`Atomics.wait` worker hangs (W-A: engine freeze;
   W-B: pool thread starvation). Mitigate: worker cap, JSC watchdog later.
   **Low-medium.**
7. (W-B) 4GB SAB reservation failure on some hosts → 2GB fallback.
   **Low.**
8. (W-B) Memory-growth + threads view-refresh subtleties in the blit/probe
   paths. **Low**, verify at prototype.

Cross-check: a Codex review (2026-06-11) verified every file:line claim in
this doc against the sources (UseMainThread plumbing, sync-load spin path,
async start/termination paths, dispatchSync wrong-thread risk, sysroot
pthread-ABI) — all confirmed — and contributed the WorkerGlobalScope
crypto-semaphore finding now covered by W-A patch hunk 4.

## Phasing & exit criteria

**Phase W-A (GO now, ~1–2 days):**
1. Patch the 3 hunks + stubs (behind `Settings`/env knob if cheap; the
   throw-removal alone is what sites observe).
2. Incremental WebCore rebuild (no flags change → minutes, not hours).
3. Gates: all existing gate pages green, sweep regression (5 sites) clean.
4. Empirical answer: does the 2captcha/reCAPTCHA worker RUN in main-thread
   mode (no NotSupportedError, no importScripts NetworkError, challenge
   variant changes)? Log worker creation + script URLs.
5. Export ledger, commit, update this doc's Status.

**Phase W-B (HOLD — trigger = W-A shows importScripts/parallelism is
required, or a target site needs real worker threads):**
1. `BIB_THREADS=1` → `build/webcore-mt` (new build dir; flags above).
2. Pre-flip fixes: dispatchSync thread check (#2), cross-thread wake (#4).
3. Full gates + sweep + perf baselines on MT build; A/B against ST build.
4. Ship decision: MT becomes default only if perf regression <10% and
   sweep is no worse; otherwise MT stays an opt-in artifact.

**Not in scope either phase:** SharedWorker, ServiceWorker, OffscreenCanvas,
guest WebAssembly (separate epic; the actual gate for Turnstile/Discord).
