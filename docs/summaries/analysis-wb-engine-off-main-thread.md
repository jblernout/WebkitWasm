# W-B scoping pass: engine off the host main thread (pthread)

Date: 2026-06-12 (task #63). Analysis only — no build flip. Modeled on
the #40 workers scoping and #53 IPInt scoping passes.

## Problem

The engine (WebCore + JSC CLoop + W-A main-thread workers + synchronous
binaryen wasm2js translations) runs ON the host page's main thread. On
bundle-heavy sites (discord.com/login is the canonical case) boot pegs
that thread for tens of seconds: white canvas, Chrome's "wait or close"
dialog, no input. Every functional Discord blocker is now fixed (A1/A2
media, WS-1 sockets, NetworkError understood); **this peg is the last
wall between us and the login form.** No amount of in-engine
optimization removes it — CLoop must execute the bundle somewhere, and
"somewhere" must stop being the UI thread.

## Target architecture

- **Engine pthread**: `-pthread` + `-sPROXY_TO_PTHREAD` — main() (and
  the whole WebCore/JSC world) moves to a dedicated Web Worker backed by
  SharedArrayBuffer. COOP/COEP are already served (hard constraint from
  day one, dev server + docs), so SAB is available today.
- **Host main thread** becomes a thin broker: input capture → proxy to
  engine; pixels ← engine; wisp WebSocket I/O (see Open Question 1);
  binaryen wasm2js moves INTO a worker context where synchronous
  translation is legal and harmless.
- **Single-mutator rule stays satisfied**: still exactly one VM on
  exactly one thread. The Thread::suspend/conservative-GC hazard
  (playbook hard constraint) does not change. ENABLE_SAMPLING_PROFILER
  stays OFF.
- The engine thread CAN BLOCK legally. Futex waits actually sleep on a
  pthread (they spin-hot on the main thread). This un-bans a whole
  class of upstream code we patched around.

## What W-B buys beyond "login stops freezing"

- Host tab stays responsive at 60fps regardless of guest JS cost; the
  kill switch banner, urlbar, and devtools stay usable mid-boot.
- Sync XHR becomes implementable (engine thread may block on the
  proxied network) — currently hard-failed in the loader strategy.
- W-A main-thread workers stop competing with the page for the UI
  thread (they still share the ENGINE thread — real worker threads are
  a separate later step W-C, now unlocked by the same -pthread flip).
- The upstream curl worker threads could come back as REAL threads,
  retiring two of our largest patch blocks (see inventory) — optional,
  the main-thread pumps also still work under -pthread.

## Work inventory

### 1. Build-flag flip (the dangerous part)

`-pthread` recompiles EVERYTHING (WTF/JSC/WebCore/Skia/deps all gain
atomics + TLS). Known consequences to audit:

- **All 71 `__EMSCRIPTEN__` guards in webkit-emscripten.patch were
  written assuming "Emscripten == single-threaded"**. With -pthread that
  equation breaks. Each guard needs a disposition: keep (true platform
  difference), re-gate on a new `BIB_SINGLE_THREADED` define (pump
  reworks), or retire (Thread::create now works). Thread-sensitive set:
  ThreadingPOSIX.cpp (sigsuspend), WorkQueue/WorkQueueGeneric
  (main-RunLoop backing), RunLoopGeneric (wakeup hook — KEEP, the
  event-driven pump is wanted on the engine thread too),
  CurlRequestScheduler + CurlStreamScheduler + CurlStream (main-thread
  pumps + multi-connect — keep OR revert to upstream worker threads),
  AsyncFileStream, IDBBindingUtilities serialization thread,
  ImageFrameWorkQueue, Worker*/WorkerRunLoop (W-A).
- Emscripten pthreads need a **pre-sized pool**
  (`-sPTHREAD_POOL_SIZE`) or thread creation blocks on a main-thread
  round trip; PROXY_TO_PTHREAD adds one automatically for main(). Start
  pool=4 (engine + headroom for W-C/curl threads later).
- wasm32 stays; 4 GB ceiling unchanged. Memory becomes a
  SharedArrayBuffer (`-sSHARED_MEMORY`); ALLOW_MEMORY_GROWTH with
  shared memory is SUPPORTED in emscripten 6.x but grows are more
  expensive (growable SAB requires browser support — Chrome yes,
  Firefox yes; Safari verify in spike).
- Stack/heap link flags carry over unchanged (8MB stack already set,
  DEFAULT_PTHREAD_STACK_SIZE=2MB already in the playbook recipe).

### 2. Host↔engine JS plumbing split (browser.html)

Every EM_ASM / Module.* touchpoint executes on the CALLING thread —
after the flip that's the worker's JS scope, where browser.html's
closures don't exist. Pthread workers get a SEPARATE, minimal Module;
custom fields are NOT replicated automatically. Inventory (12 hooks +
3 subsystems):

| Hook | Today | Under W-B |
|---|---|---|
| bibWakeUp / bibArmTimer (engine pump) | main-thread MessageChannel | trivial: worker-local setTimeout/MessageChannel — pump gets SIMPLER |
| bibWasm2js + bibWasmPolyfill (S-A shim) | sync EM_ASM → main-thread binaryen | binaryen loads in the WORKER; sync translation becomes legal (no UI jank) — strict upgrade |
| bibGetProc / bibCanvasGPU / GPU present | main-thread WebGL2 ctx | OffscreenCanvas transferred to worker (`-sOFFSCREENCANVAS_SUPPORT`); see §3 |
| blit (raster) | HEAPU8 view → putImageData | worker posts dirty rects (or transfers ImageBitmap); main thread draws. HEAPU8 is the SAB — main thread can read it directly, zero-copy stays possible |
| input (bib_mouse/bib_key ccall) | direct ccall | `emscripten_dispatch_to_thread`-style proxy or postMessage→worker ccall; PROXY_TO_PTHREAD gives proxying queues for free |
| bibInteractive / bibHTML / bibCurlDebug / bibNoBlock / bibGPU flags | read once at boot via EM_ASM | serialize into worker Module at spawn (mainScriptUrlOrBlob preamble or pthread-main message) |
| bibGpuFallback / onEngineReady / onAbort | C → main-thread JS | emscripten proxies main-thread calls via `MAIN_THREAD_EM_ASM` — mechanical rename |
| wisp WebSocket dispatcher | page-scope WebSocket override | **see Open Question 1** — the load-bearing unknown |
| console/printErr forwarding | main-thread printErr | pthread stdout routes to main thread automatically (proxied) — verify ordering |
| kill switch (__bib.dead) | main-thread guards | main thread stays alive even if engine thread dies — kill switch gets MORE reliable (banner can't be starved) |

### 3. GPU mode (G2/G3 work preserved?)

OffscreenCanvas + `-sOFFSCREENCANVAS_SUPPORT` +
`-sOFFSCREENCANVASES_TO_PTHREAD=#canvas` moves the WebGL2 context to
the engine thread; Ganesh/GrDirectContext code is thread-agnostic (it
already runs wherever the context is current). Risks: (a) Safari
OffscreenCanvas+WebGL2-in-worker support is the weakest link — raster
mode remains the fallback (blit via SAB is actually EASIER under W-B);
(b) G4 context-loss recreate interacts (loss events arrive on the
worker's canvas proxy — fold G4 into W-B2 rather than doing it twice).

### 4. Open questions for the W-B0 spike (each ~hours, not days)

1. **SOCKFS WebSocket placement under PROXY_TO_PTHREAD** (the
   load-bearing one): does the WebSocket get created on the engine
   pthread's worker scope (then our wisp dispatcher must be installed
   in that scope — doable via worker preamble) or proxied to the main
   thread (dispatcher stays put, but every socket op pays a proxy
   hop)? Emscripten has both modes historically
   (PROXY_POSIX_SOCKETS vs in-worker SOCKFS). Spike: 50-line pthread
   hello-world doing a fetch over our wisp shim, instrumented in both
   scopes.
2. ALLOW_MEMORY_GROWTH + SHARED_MEMORY in all three target browsers
   (growable SAB); fallback = pre-size INITIAL_MEMORY=512MB-1GB.
3. OffscreenCanvas WebGL2 in worker on Safari/Brave (G3 measured GPU
   wins on Chrome-class; raster fallback acceptable elsewhere).
4. Atomics tax on CLoop: -pthread makes WTF locks real atomics —
   measure gate-suite + perf-probe delta single-threaded-pthread vs
   today (expect low single-digit %; abort if >10%).
5. Pthread stdout/printErr ordering + abort-stack symbolization (does
   --profiling-funcs survive the worker boundary in pageerror.stack?).

## Risks

- **Full-rebuild + patch-audit blast radius**: this touches the whole
  patch file and every build artifact. Mitigation: branch + keep the
  single-threaded build path alive behind the existing flags until
  W-B2 validates (the embedder.cmake flip is one block; OptionsXXX
  unchanged).
- **Safari**: SAB requires COOP/COEP (we have it) but OffscreenCanvas
  WebGL2-in-worker may force raster mode there. Acceptable — raster is
  today's default for harness runs anyway.
- **Latency regressions**: input now crosses a thread hop (~0.1-1ms,
  noise vs current event-driven pump gains); network ops may pay proxy
  hops depending on Q1's answer.
- **The unknown unknowns of -pthread WebKit-on-wasm**: nobody ships
  this configuration; expect a playbook-chapter's worth of new traps.
  That was also true of every phase so far.

## Recommendation: GO, phased — confidence: medium-high

The peg is structural and W-B is the only lever that removes it; all
cheap levers are exhausted as of tonight (blocklist measured as a
non-factor for /login). The single-mutator hazard — the original reason
W-B was HOLD — is fully sidestepped by keeping ONE engine thread.

- **W-B0 (spike, ~1 session)**: minimal -pthread+PROXY_TO_PTHREAD
  hello-world answering Open Questions 1/2/5 OUTSIDE WebKit (gpu-spike
  pattern). Kill criteria: wisp-over-SOCKFS unworkable from pthread, or
  growth-SAB broken in two of three browsers.
- **W-B1 (raster bring-up, 1-2 sessions)**: full -pthread rebuild,
  patch-audit (the 71 guards), browser.html split, input proxy, SAB
  blit. Gate: gates 2/3/9 green + discord.com/login leaves the host
  tab responsive while booting (THE acceptance).
- **W-B2 (GPU, 1 session)**: OffscreenCanvas move + fold G4
  context-loss recreate in. Gate: gate8 + M2-class GPU numbers.
- **W-B3 (dividends, opportunistic)**: real curl threads (retire two
  patch blocks), sync XHR, W-C real worker threads.
