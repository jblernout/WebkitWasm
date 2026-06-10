# Handoff — Phase 5: browser chrome + Phase 4 leftovers (the ONE active handoff)

**Written**: 2026-06-10 ~04:55 EDT, right after Phase 4 core completed.
**Updated**: 2026-06-10 ~17:20 EDT after the IDB + diagnostics + rAF
session. Day's commits: … → aa5cda7 (cookies) → 9c9e8ef (site support) →
4db3dbf (recaps) → cc7794a (IndexedDB + gate7) → ff6ff82 (network
diagnostics) → 810bdfd (diagnose-tool fix) → rAF/RIC/bib_eval chunk
(git log tip; suite 9/9 throughout).
**Supersedes**: handoff-2026-06-10-phase4-networking.md (→ docs/archive/).

## ⟶ NEXT SESSION STARTS HERE: PERFORMANCE + site-support wave 2
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
