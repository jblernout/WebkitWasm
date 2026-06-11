# Handoff — Phase 4 usability: site support + performance (the ONE active handoff)

**Written**: 2026-06-10 ~04:55 EDT, right after Phase 4 core completed.
**Updated**: 2026-06-10 ~23:30 EDT after the woff2-resolution /
happy_wheels-crash session. Day's commits: … → cc7794a (IndexedDB) →
ff6ff82 (network diagnostics) → 810bdfd (diagnose fix) → b4ee171 (rAF
#16) → ff8c524 (event-driven pump #17) → a1d6dec (Chunk B fonts/_blank/
ping) → woff2-resolution commit (git tip; latin pacifico + matrix probe +
diagnostics stripped).
**Supersedes**: handoff-2026-06-10-phase4-networking.md (→ docs/archive/).

## Honest phase status (user-set framing, 2026-06-10 22:30)
**Phase 4 is NOT done.** The core pipeline browses the real web (TLS,
cookies, storage, IDB, images, iframes, rAF), but "browses" ≠ "usable":
performance on real sites and breadth of site support ARE the remaining
bulk of Phase 4. The pump fix (#17) is one large step, not the finish —
nothing below is done until real sites feel usable and the wave-2 sweep
verdicts flip. Phase 5 chrome (title/back-forward/etc.) stays SECONDARY
to that.

## ⟶ NEXT SESSION STARTS HERE: Workers Phase W-A (main-thread workers)

Task #40 (Workers scoping pass) is **DONE** (2026-06-11 ~00:15) —
deliverable: **docs/summaries/decision-004-workers-scope.md**. Verdict:
**GO on Phase W-A** (upstream `WorkerThreadMode::UseMainThread` for
dedicated workers — NO pthread flip, ~4-hunk WebKit patch + fail-fast
stubs for importScripts/sync-XHR + the WorkerGlobalScope crypto-semaphore
sites Codex found, incremental rebuild, 1–2 days);
**HOLD on Phase W-B** (real pthread workers, 1–2 weeks) until W-A's
empirical reCAPTCHA result. Headline findings (details + file:line
evidence in the doc):
- Upstream already runs workers WITHOUT an OS thread (service-worker
  testing mode): `WorkerMessagingProxy.cpp:162` is the switch;
  main-thread workers share commonVM (zero per-worker VM cost).
- importScripts/sync-XHR would INFINITE-SPIN in that mode (sync load pumps
  the C++ RunLoop while wisp bytes need host JS events) → stubs mandatory.
- The wasm-sysroot is ALREADY pthread-ABI (atomics+bulk-memory verified in
  libsqlite3/libcurl/libicuuc/libfreetype target_features) — a W-B flip
  recompiles only the WebKit tree + embedder, into a separate
  build/webcore-mt dir. embedder.cmake:34's "sysroot is single-threaded"
  comment is stale/wrong.
- W-B pre-flip MUSTs: dispatchSync wrong-thread fix + a cross-thread
  main-RunLoop wake (worker→main callOnMainThread currently couldn't wake
  the host pump).
- Unlock truth: reCAPTCHA is the ONLY sweep-verified site where Workers is
  the binding gap; Turnstile/Discord stay gated on guest wasm regardless.

Next concrete step (pending user go on the doc's recommendation):
implement W-A per the doc's "Phasing & exit criteria" — 4 hunks + stubs,
incremental rebuild, gates + 5-site sweep regression, log the reCAPTCHA
worker's behavior. Export ledger after WebKit edits; commit.

**Wave-2 sweep (task #39, 2026-06-10 ~23:45) — the checkup that picked
this priority. Every verdict improved; NO aborts, NO crashes:**
- **old.reddit FLIPPED**: real front page renders (941 colors, 7 streams,
  zero errors) — the old server-side "network policy" block page is gone.
- **google login FULL FLOW WORKS**: email visible while typing, Enter →
  real account-validation error rendered in ≤3s. The never-resolving
  gray overlay is dead (pump #17).
- **reCAPTCHA click-feel FIXED**: click→first visual response **0.46s**
  (was "slow after click"); the full image challenge (3×3 grid, all
  tiles painted) renders. Workers NotSupportedError fires on load AND
  +2.3s post-click — the challenge path wants a Worker; Workers remain
  the suspected fallback-variant trigger.
- **velzie.rip near-perfect**: fonts/pfp/now-playing all render; ONLY the
  WebGL background is dead (gl.viewport null, #32). Button-wall failures
  are environmental (LAN ad-block + rinici.de bot-filter).
- **fal.ai/grants renders fine**; one purple-noise artifact in their
  animated hero canvas (minor, unexplored).
- Only TWO engine gaps repeated across all five sites: **Web Workers**
  (×2) and **WebGL** (×2). Everything else environmental.
- Perf calibration: pump-era numbers hold (MessageChannel hop 0.38ms,
  setTimeout hop 6.93ms, fetch 44ms). Click-feel is no longer the
  problem → dirty-rect (#37) is an efficiency win now, not a feel fix.
- Sweep ran with BIB_CHANNEL=chromium (knob added to site-diagnose.mjs +
  google-login-repro.mjs, commit e48bd55) — zero host crashes, #38
  mitigation works. recaptcha click runner: build/recaptcha-click.mjs
  (gitignored). Logs: build/sweep2-*.log, screenshots build/diagnose-*,
  build/google-login-*, build/recaptcha-*.

**Closed this session (don't re-litigate):**
- **woff2 "bug" RESOLVED — NOT a bug.** The vendored pacifico.woff2 was
  Google Fonts' CYRILLIC-EXT subset (cmap = space/NBSP/CR + 278 Cyrillic
  codepoints, zero Latin). Engine per-char fallback was CORRECT the whole
  time (space rendered from Pacifico — the 432-vs-434 width tell). Latin
  subset vendored → web/probe/webfont-matrix.html 5/5 PASS: woff2+ttf,
  CSS @font-face + JS FontFace all apply real glyphs. WOFF2/custom-font
  support is DONE and verified. All temp font diagnostics stripped; tree
  == committed ledger (re-export produced zero diff). Lesson recorded in
  the memory playbook: verify test-asset cmaps (fonttools ttx -t cmap)
  before debugging the engine; vendor the LATIN subset URL, not the
  first @font-face block Google serves.
- **happy_wheels.tjf "abort" (user todo) DIAGNOSED, two findings**:
  (a) the game itself needs WebGL (PixiJS: "Can't find variable:
  WebGLRenderingContext") — same gap as velzie.rip (#32); (b) the abort
  is a HOST browser crash: chromium-headless-shell-1223 renderer dies
  SEGV_ACCERR with crash IP in V8 JIT code space, ~1/3 of loads, ~8-25s
  in, while the engine runs the game's 4.2MB JS bundle in CLoop. NOT
  OOM (cgroup flat ~870MB), NOT transfer corruption (in-engine fetch of
  the bundles = byte-perfect 3/3 vs local truth). --no-wasm-tier-up
  REDUCED but did not eliminate (1 crash/8 rounds vs ~2/5 baseline).
  Full-chromium + Firefox cross-checks were mid-run at session end
  (build/hw-fullchromium.log). Verdict so far: host V8 bug triggered by
  our workload — NOT an engine-code bug; candidate upstream report once
  isolated. Repro harness: build/hw-crash-catch.mjs (BIB_CHROME_FLAGS /
  BIB_CHANNEL env knobs), logs build/hw-*.log.

**The menu for "more usable" (re-ranked after the sweep):**
1. ~~Wave-2 real-site sweep (#30)~~ DONE 2026-06-10 (see verdicts above).
2. **Web Workers epic** ← CURRENT (scoping pass first, task #40) —
   single biggest site-support unlock left:
   gates Discord, Cloudflare Turnstile, likely the reCAPTCHA fallback-v2
   downgrade (2captcha logs "Web Workers are not supported"), and
   degrades countless SPA loaders. Safe under the one-mutator-per-VM
   constraint (each Worker = own VM on own pthread) but means flipping
   the single-threaded build to -pthread = FULL rebuild + re-audit of
   every single-threaded surgery point in the ledger. BIG; scope first.
3. **Dirty-rect paint/blit (#37 remainder)** — every dirty frame is a
   full 800×600 repaint + 1.92MB readPixels + putImageData. Biggest
   steady-state CPU win for interactive feel on busy pages.
4. **Page-level WebSocket (CurlStream rework)** — currently fails by
   design (blocking CONNECT_ONLY can't finish single-threaded). Same
   main-thread pump pattern as CurlRequestScheduler. Unlocks chat/live
   sites.
5. **WebGL (#32)** — null context kills velzie.rip + happy-wheels game
   + any PixiJS/three.js site. Real scope: GraphicsContextGL for the
   port (Skia Ganesh→WebGL2 or ANGLE-on-WebGL2); investigate before
   sizing.
6. **Host devtools CPU profile of bib_tick** on a slow real site —
   if sites still feel sluggish after 1-4, profile before optimizing
   further.
7. **Phase 5 chrome** (title/URL/progress, back-forward capacity-0) —
   SECONDARY per user framing.
**Parked**: guest-wasm epic (Discord/Turnstile hard-gate on guest
WebAssembly too — Workers alone may not save them). curl=35/92
closed-environmental. happy_wheels host-V8 crash (watch, report
upstream when reduced).

## Previous session (rAF #16): PERFORMANCE + site-support wave 2
Root causes #13–16 are DONE this session: IndexedDB (in-process server),
RequestIdleCallbackEnabled (yaml default false), and the big one — guest
requestAnimationFrame NEVER fired (no DisplayRefreshMonitor; bib_tick now
drives Page::updateRendering() + finalizeRenderingUpdate({}) per host
frame). That one fix made the 2captcha reCAPTCHA widget render (api.js
live from www.google.com), fixed google login's floating label (#28
CLOSED), and unstuck everything animation/rAF-shaped.

1. **PERFORMANCE (top priority — user-flagged again at wrap-up)**:
   input latency, slow loads, "slow after click" on the reCAPTCHA.
   RE-BASELINE FIRST: the rAF fix changed the profile (rendering-update
   steps now run every tick — check updateRendering isn't doing wasted
   style/layout per frame on idle pages). Then profile the pump:
   bib_tick (RunLoop::cycle + updateRendering) vs paintFrame raster vs
   curl pump cadence. Tools: __bib.eval(js) for guest-side timing
   (performance.now deltas, long tasks), ?curldebug=1 for network
   timing, host-side devtools profiling (wasm frames have names).
2. **reCAPTCHA fallback (#34 tail, user observation at wrap-up)**: the
   widget works but is SLOW after click and serves a *partially*
   working FALLBACK v2 variant, not the regular one — Google downgrades
   on missing features/timing. Find the downgrade trigger via bib_eval.
   May be the same story as item 1 (slowness IS the trigger).
3. **Site support wave 2 (#30)**: re-sweep the day's problem sites —
   the rAF fix likely changed verdicts everywhere (anything "stuck" may
   work now). site-diagnose over: fal.ai, velzie.rip, google login full
   flow, old.reddit, others from user browsing. Remaining deliberate
   gaps: SharedWorker, WebAssembly, caches, serviceWorker.
4. **Phase 5 chrome**: title/URL/progress callbacks; back/forward
   (EmptyBackForwardClient capacity 0 = next silent gate); optional
   OPFS persistence for cookies + web storage.
5. **Parked**: guest-wasm epic (wasm3/WAMR interpreter +
   window.WebAssembly bridge + pseudo-workers) unlocks discord-renders
   + Turnstile-runs (not necessarily -passes; adversarial). Multi-week;
   decision doc first. curl=35/92 CLOSED-ENVIRONMENTAL: LAN ad-block
   DNS blackholes tracking hosts to 0.0.0.0 (verify any "SSL connect
   error" with getent + the new wisp CLOSED-reason log lines before
   suspecting the engine); rinici.de bot-filters curl-family clients.

## rAF/diagnostics session (2026-06-10 ~15:00–17:20)
- **IDB (#13)**: BibIDBServer.{h,cpp} = WebKitLegacy InProcessIDBServer
  carried into the embedder + BibDatabaseProvider; IDBBackingStore.h
  ctor/dtor RELEASE_ASSERT(!isMainThread()) guarded out (ledger).
  gate7-idb 4/4. Trap: new embedder .cpp MUST start with config.h.
  Discord: script survives to late bootstrap; page blank = libdiscore
  needs guest wasm (epic). Turnstile demo page renders; widget
  correctly self-reports unsupported (same boundary).
- **DEBUG_CURL was dead three ways** (all fixed): NDEBUG guards in
  CurlContext.{h,cpp}; CurlRequest's CURLOPT_DEBUGFUNCTION swallowed
  verbose output (re-emits CURLINFO_TEXT now); env must be set via
  setenv at TOP of main() (CurlContext singleton reads it once during
  installEmbedderStrategies). ?curldebug=1 → full TLS/h2 traces.
- **bib_eval(js)** export + __bib.eval() helper: guest-world JS probes,
  output via console forwarder. No more rebuilds for DOM questions.
- **Probe pages**: web/probe/{features,modules,vitepreload}.html.
  site-diagnose prints head+tail of guest console (810bdfd — head-only
  cap faked a "hang" and burned an hour; trust but verify the TOOLS).
- **Host OOM 16:30**: zellige-ai's next-server (15GB anon-rss, kernel
  log) — NOT this project. Cap external dev servers with systemd-run.

## Site-support session (2026-06-10 ~14:00–14:50) — commit 9c9e8ef
Diagnosis loop: --profiling-funcs (name section, +12MB wasm; abort
stacks now symbolized) + BibChromeClient::addMessageToConsole →
WTFLogAlways → "[bib] err: BIB: console …" (relaxed final, ledger).
tools/site-diagnose.mjs groups guest-console lines.
- fal.ai/grants ABORT root cause: blob teardown →  ~AsyncFileStream →
  callOnFileThread → std::call_once → Thread::create → RELEASE_ASSERT.
  Fixed in-tree under __EMSCRIPTEN__: main-RunLoop dispatch (FIFO
  preserved). Same sweep fixed callOnIDBSerializationThreadAndWait
  (inline + static NeverDestroyed context — would abort THEN deadlock).
  Thread::create audit: GC-controller path is debug-only; audio/video/
  webrtc compiled out; workers already guarded; WebSQL+SW default off.
- Root cause #11 (storage): LocalStorageEnabled/SessionStorageEnabled
  default FALSE at the WebCore layer (IDL-gated → ReferenceError, fatal
  to site bootstraps) AND EmptyStorageNamespaceProvider discards writes.
  src/embedder/BibStorage.h: StorageMap-backed areas, per-origin, 5MB;
  settings flipped in main.cpp. In-memory lifetime (= cookies policy).
- Root cause #12 (subframes): EmptyFrameLoaderClient::createFrame
  nullptr → iframe contentWindow null (killed discord bootstrap, hung
  google's CheckConnection). BibFrameLoaderClient::createFrame per the
  WebKitLegacy recipe (createSubframe + setSpecifiedName + init +
  page-null recheck) + transitionToCommittedForNewPage per the WebKit2
  recipe (view replaced EVERY commit; size + canHaveScrollbars carried
  over). main.cpp: Engine no longer caches LocalFrameView —
  mainFrameView() lookup each use (a cached view goes stale/blank after
  first navigation); boot re-fetches after init() and writer-load.
- VERIFIED: suite 8/8 (offscreen gate byte-identical exactBlue=20000
  redGlyph=1541), fal.ai renders w/o abort, google login Enter →
  real account-validation response, HN smoke 4/4. Codex: 0 high/med.
- Memleak note: user's reboot mid-session — likely the wasm link
  pipeline (metadce/wasm-opt spike multi-GB with -g). ALL builds and
  browser runs now under systemd-run scopes (12G build / 8G browser).

## Cookies session (2026-06-10 afternoon) — DONE, gate6 3/3
Root cause was the empty-clients family AGAIN (#10):
pageConfigurationWithEmptyClients installs CookieJar over
EmptyStorageSessionProvider (NULL NetworkStorageSession) → document.cookie
writes vanish, reads return "" → google's "Cookies are disabled"
interstitial. AND the network path never attached/stored cookies.
- src/embedder/EmbedderStrategies.cpp: embedderStorageSession() —
  NetworkStorageSession(defaultSessionID) with
  setCookieDatabase(CookieJarDB ":memory:") (in-memory sqlite; curl port
  ctor would use a MEMFS file path); EmbedderStorageSessionProvider +
  createEmbedderStorageSessionProvider() for main.cpp; BibResourceLoad
  gained appendCookieHeader() (in createCurlRequest — initial + every
  redirect hop) and storeResponseCookies() (in curlDidReceiveResponse
  BEFORE the redirect check, so Set-Cookie on 3xx legs sticks). Both are
  verbatim NetworkDataTaskCurl recipes (appendCookieHeader /
  handleCookieHeaders).
- src/embedder/main.cpp: pageConfiguration.cookieJar =
  CookieJar::create(createEmbedderStorageSessionProvider()).
- CookieJarDB ":memory:" is a first-class mode (isOnMemory()); accept
  policy defaults to Always; no threads/locks inside — single-thread safe.
- NEW GATE: tools/gate6-cookies-test.mjs + web/gate6/cookies.html
  (document.cookie round-trip, navigator.cookieEnabled, network
  Set-Cookie→Cookie echo via /cookie-test/set + /cookie-test/echo
  endpoints added to tools/dev-server.mjs). 3/3.
- VERIFIED LIVE: google.com renders the REAL homepage (logo, search box,
  Sign in — build/diagnose-https-www-google-com.png). old.reddit still
  serves its "network policy" block page — that's server-side
  (wisp-exit IP reputation / UA heuristics), NOT cookies; engine renders
  the block page correctly. Possible later experiment: tweak
  standardUserAgent().

## State (Phase 4 core COMPLETE — all gates green)
- **THE ENGINE BROWSES THE REAL WEB.** GATE 4 3/3 + MODERN-SITE SMOKE 4/4:
  https://news.ycombinator.com fetched live over Wisp, TLS terminated
  in-engine (OpenSSL + CA bundle), CSS subresource applied (#ff6600
  header), full page painted. Proof: build/smoke-modern-site.png,
  build/https-test.png (example.com). Offscreen/gate2/gate3 unregressed.
- **Load pipeline**: bib_load_url(url) → FrameLoader::load →
  BibFrameLoaderClient (policy Use, committedLoad→commitData) →
  CachedResource → BibLoaderStrategy::loadResource → SubresourceLoader →
  BibResourceLoad (CurlRequestClient; redirects ≤20, GET-conversion,
  cross-origin header stripping) → CurlRequest → CurlRequestScheduler
  (main-thread pump under __EMSCRIPTEN__) → SOCKFS → page WebSocket
  dispatcher → WispWebSocket (web/vendor/wisp-client.js, wisp-js 0.4.1)
  → wisp server. data: URLs → loader->start() (pre-ResourceHandle path).
- **Codex review hardening (ed77c77)**: (a) Location: file:/// redirects
  REJECTED in BibResourceLoad::performRedirect (remote page could read
  MEMFS — CA bundle/fonts — as content); (b) null willSendRequest
  completion cancels the parked CurlRequest (was leaking it paused);
  (c) the WebSocket dispatcher only reroutes sockets carrying the
  'bib-sockfs' marker subprotocol (Module.websocket.subprotocol) — page
  sockets can't be false-positive routed onto Wisp by URL shape.
- **Run the stack** (three processes):
  - `node tools/dev-server.mjs web --mount /engine=build/webcore/bin`
  - `npm run wisp` (wisp-js-server on 5001, loopback/private allowed — DEV
    ONLY setting, an SSRF hole on anything public)
  - open `http://127.0.0.1:8080/browser.html?url=https://...`
    (`&curldebug=1` → libcurl verbose to the page log… NOTE: only in
    non-NDEBUG builds; release ignores it)
- Gates: tools/run-embedder.cjs, gate2/gate3/gate4-browser-test.mjs,
  gate5-images-test.mjs, gate6-cookies-test.mjs, urlbar-test.mjs,
  smoke-modern-site.mjs (parameterized: `node tools/smoke-modern-site.mjs
  https://any-site/`).
- WebKit patch ledger 1851 lines (src/patches/webkit-emscripten.patch),
  includes: CurlRequestScheduler pump, CurlRequest::runOnMainThread defer,
  EmptyFrameLoaderClient.h final→override ×5, ENABLE_FTPDIR=OFF
  (OptionsEmscripten), CurlSSLHandle/NetworkStateNotifier/UserAgent
  Emscripten platform files, EmptyClients.h scroll-damage overrides,
  ScrollAnimator guard-join, PlatformKeyboardEvent half.

## Phase 4 root causes (memory playbook "Networking root causes" has detail)
1. EmptyFrameLoaderClient: policy completions DROPPED, canHandleRequest
   false, canShowMIMEType false, committedLoad no-op → 4 silent kills.
2. ENABLE_FTPDIR=ON → CURLOPT_PROTOCOLS_STR "file,ftp,…" → curl
   protocol2num zeroes allowlist, fails mid-parse on unbuilt ftp → only
   "file" allowed → http "Unsupported protocol". Port option now OFF
   (cache override needed on existing build dirs: -DENABLE_FTPDIR=OFF).
3. curl ResourceHandle does not exist in 2.52 (start() asserts) — loader
   strategies MUST intercept; ResourceLoader's public feeding interface
   is the supported WK2-style path.

## Memory investigation (2026-06-10 evening — user's Firefox lockup)
**Verdict: NO leak. The lockup was rapid-reload instance stacking in
Firefox.** Evidence (tools/memwatch.mjs, tools/reloadwatch.mjs — keep both):
- Steady-state CLEAN in Chromium AND Firefox: wasm heap flat at 256 MB
  through 60s idle + 60s input storm + 6 renavigations; 0 idle blits;
  JS heap flat; dev/wisp servers tiny after hours.
- Reload behavior: each browser.html load = ~1 GB (256 MB wasm heap +
  compiled code for the 89 MB module; Firefox tab baseline ~1.5 GB).
  Firefox reclaims old instances LAZILY (~30-60s+): reloads spaced 60s
  apart stay bounded (~2.4 GB peak); reloads every ~3s STACK old
  instances (4+ GB after 3 reloads) → swap-thrash → lockup (2 GB swap).
  Chromium tears down within a cycle or two (bounded ~1.8 GB).
- Fixes landed: dev-server `no-cache` + ETag/304 (was `no-store` — full
  89 MB re-download + full recompile per reload; real-profile Firefox
  and Chromium now revalidate; NOTE Playwright's ephemeral FF profile
  has no http cache, so reloadwatch still shows 85M transfers there);
  browser.html log capped at 20 KB; unload/pagehide handlers (bfcache
  block + Module root drop — bfcache was NOT the retention mechanism,
  kept as hygiene).
- RULE for memory tests: ALWAYS run browser memory tests inside
  `systemd-run --user --scope -p MemoryMax=8G -p MemorySwapMax=0 --` —
  an uncapped reload test locked up the host machine once.
- Dev workflow advice: space engine-page reloads ~30s+ in Firefox, or
  test in Chromium.

## Abort-hunt session (2026-06-10 afternoon) — "complex sites abort" SOLVED
User report: pages with images/assets abort; google/ebay/2captcha abort.
All root-caused with gate5 + tools/site-diagnose.mjs (symbolized stacks
via temp -sASSERTIONS + --profiling-funcs; both REVERTED after):
- **WorkQueue thread-spawn abort**: ANY WorkQueue::create spawned a real
  thread (pthread_create=ENOTSUP → Thread::create RELEASE_ASSERT →
  abort). First page with an <img> died in ImageFrameWorkQueue. PATCH:
  WorkQueueGeneric.cpp backs every queue with the MAIN RunLoop;
  WorkQueue.cpp dispatchSync runs inline; ConcurrentWorkQueue::apply
  runs serially.
- **ImageFrameWorkQueue spin-deadlock**: SynchronizedFixedQueue
  (BufferSize=8) blocking enqueue/dequeue can't be satisfied by another
  thread — >8 pending decodes spun the only thread at 100% CPU forever
  (Wikipedia). PATCH: dispatch() decodes via callOnMainThread, no queue.
- **Root cause #8 — loadsImagesAutomatically defaults FALSE** in raw
  WebCore: every non-data: image load silently DEFERRED (no request, no
  error, blank <img>). FIX: setLoadsImagesAutomatically(true) in
  main.cpp settings block.
- **Root cause #9 — createBlobRegistry() returned nullptr** (our own
  deferred stub): first `new Blob(...)` any modern bundle ran →
  blobRegistry() CheckedRef null → abort. google/ebay/Turnstile all died
  here. FIX: EmbedderBlobRegistry forwarding to in-process
  BlobRegistryImpl (WebKitLegacy WebBlobRegistry pattern) + blob: loads
  routed through loader.start() (BlobResourceHandle builtin map).
- **Worker::create graceful-fail**: throws NotSupportedError instead of
  aborting the engine (WorkerThread::start would Thread::create).
  SharedWorker is settings-gated OFF already; ServiceWorker off.
- **In-page WebSocket**: CurlStreamScheduler got the main-thread pump
  (same recipe as CurlRequestScheduler) BUT CurlStream's blocking
  CONNECT_ONLY perform() can never complete single-threaded (the SOCKFS
  WebSocket needs the JS loop to open) → CurlStream now fails the stream
  immediately = pages get clean WebSocket error events. REAL in-page WS
  needs a multi-interface CurlStream rework (deferred).
- RESULTS: gate5-images 3/3 (PNG+GIF+data:), Wikipedia Main Page renders
  WITH images (build/diagnose-…wikipedia….png), eBay homepage renders
  (hero, logos, search UI; 23 wisp streams), google + 2captcha no longer
  abort. old.reddit = engine-correct, server bot-blocked (needs cookies).
  Benign leftover: "missingImage" platform resource warning (no broken-
  image icon bundled).
- NEW TOOLS: tools/gate5-images-test.mjs (now a standard gate),
  tools/site-diagnose.mjs (abort/paint/stream classifier),
  tools/urlbar-test.mjs. web/gate5/ fixtures (valid 180-byte PNG,
  226-byte GIF, images.html with onload/onerror signal divs).
- Empty-client ledger grew: IDB requests DANGLE silently
  (EmptyDatabaseProvider no-op delegate) — future silent gate.
- **Codex review round 2** (commits 8b46572+2ea0200; 0 crit/high, 1 med,
  2 low): MED scheme injection (javascript:/file:/data: reached
  bib_load_url via URL bar AND ?url=) → normalizeEngineURL http(s)
  allowlist on both paths; LOW ImageFrameWorkQueue null-decoder left a
  stale request at decodeQueue head → decoder check moved before append;
  LOW ETag same-size stale-serve → DECLINED (fix would reintroduce the
  no-store reload spike; collision needs same size AND same ms mtime).

## NEXT: Phase 5 — browser chrome (plain web dev)
(Priority order now lives in "NEXT SESSION STARTS HERE" at the top.)
1. ~~URL bar~~ DONE (2026-06-10): #urlbar + Go in web/browser.html,
   Enter/click → bib_load_url in place, ?url= via history.replaceState,
   tools/urlbar-test.mjs 3/3. Still TODO: bib_stop/bib_reload, loading
   state from engine callbacks.
2. Title/URL/progress callbacks: BibFrameLoaderClient overrides
   (dispatchDidReceiveTitle, dispatchDidStartProvisionalLoad,
   dispatchDidFinishLoad...) → EM_ASM → host page events. Note: most
   dispatch* on EmptyFrameLoaderClient are final — relax as needed
   (established pattern).
3. Link navigation policy: clicks already navigate same-frame? (policy
   Use). New-window actions are Ignored — decide target=_blank handling.
4. History (back/forward): BackForwardClient is EmptyBackForwardClient
   (capacity 0) — needs a real in-memory BackForwardList for back/forward.

## Phase 4 leftovers (updated 2026-06-10 ~14:00)
- ~~Images~~ DONE — gate5-images 3/3; Wikipedia/eBay render with images.
- ~~Cookies~~ DONE — gate6-cookies 3/3; google.com renders for real
  (see "Cookies session" above). In-memory only; OPFS persistence open.
- HTTP auth (401/407), sync XHR (blocked on single-thread — document as
  unsupported), ping/preconnect completions are error-stubs.
- ~~WebSocket-in-page thread abort~~ DONE (scheduler pumped) — but
  streams FAIL IMMEDIATELY by design: CurlStream's blocking CONNECT_ONLY
  perform can't complete single-threaded. Real WS = multi-interface
  CurlStream rework (pages currently get clean error events).
- Workers: constructor throws NotSupportedError (catchable) — real
  worker support would need a big architectural lift; not planned.
- Scheduler pumps idle at rAF cadence — fine for now; event-driven
  wakeup (socket callbacks) is a perf-phase item.
- "missingImage" platform resource warning: bundle WebCore's broken-image
  icon (cosmetic).

## Known traps (carried forward)
- Empty-client semantics bite SILENTLY. Before suspecting wasm, check
  what pageConfigurationWithEmptyClients installed (sandbox flags, final
  no-op methods, false gates, dropped completion handlers, null
  providers). TEN root causes so far were all this family (#10 =
  EmptyStorageSessionProvider's null session ate document.cookie).
- WTFLogAlways probes (printErr) inside WebCore/embedder beat static
  tracing for "handled but nothing happened" — REVERT before
  export-webkit-patches.sh.
- WEBKIT_OPTION_DEFAULT_PORT_VALUE only affects FRESH CMake caches —
  existing build dirs need -D<OPT>=… override once.
- Wide ninja rebuilds can OOM-kill em++ silently ("FAILED" with only
  warnings) — just re-run ninja.
- bash `have=$(rg …)` under set-e/pipefail dies silently on no-match —
  `|| true`.
- NEVER rm -rf build/webcore. Every WebKit edit → export-webkit-patches.
  Local git only. Codex review before presenting non-trivial code.
