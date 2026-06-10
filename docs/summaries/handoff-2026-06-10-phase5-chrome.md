# Handoff — Phase 5: browser chrome + Phase 4 leftovers (the ONE active handoff)

**Written**: 2026-06-10 ~04:55 EDT, right after Phase 4 core completed.
**Updated**: 2026-06-10 ~08:30 EDT after the Codex review round (commit
ed77c77) — all findings fixed, all gates re-verified green.
**Supersedes**: handoff-2026-06-10-phase4-networking.md (→ docs/archive/).
**Session commits**: 95fe79c (Phase 4 networking complete) → ed77c77
(Codex fixes). Tree clean.

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

## NEXT: Phase 5 — browser chrome (plain web dev)
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

## Phase 4 leftovers (pick up opportunistically)
- Images: decoders linked + loader live — verify <img> renders, add to a
  gate (HN renders its gif arrows? grayarrow2x.gif didn't obviously show).
- Cookies: CookieJarDB (sqlite, linked) — NetworkStorageSession wiring +
  persistence later (OPFS). Many sites need cookies to behave.
- HTTP auth (401/407), sync XHR (blocked on single-thread — document as
  unsupported), ping/preconnect completions are error-stubs.
- WebSocket-in-page: CurlStreamScheduler::createThreadIfNoCurrentThread
  spawns a Thread — will trap/misbehave; needs the same main-thread-pump
  treatment if/when needed.
- Scheduler pump idles at rAF cadence (~60 pumps/s during load) — fine
  for now; event-driven wakeup (socket callbacks) is a perf-phase item.

## Known traps (carried forward)
- Empty-client semantics bite SILENTLY. Before suspecting wasm, check
  what pageConfigurationWithEmptyClients installed (sandbox flags, final
  no-op methods, false gates, dropped completion handlers). Seven root
  causes so far were all this family.
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
