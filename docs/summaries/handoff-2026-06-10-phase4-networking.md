# Handoff — Phase 4: networking over Wisp (the ONE active handoff)

**Written**: 2026-06-10 ~01:40 EDT, right after Phase 3 core completed.
**Supersedes**: handoff-2026-06-09-phase3-canvas.md (→ docs/archive/).

## State (Phase 3 COMPLETE — all three gates green)
- **GATE 3 PASSED 8/8**: real Playwright input on the canvas drives the
  engine — :hover restyle, JS onclick (JSC EXECUTES IN-PAGE), typed text
  lands in an `<input>` via the editor, wheel scrolling repaints. Proof:
  `build/gate3-canvas.png`. Offscreen gate byte-identical
  (exactBlue=20000 redGlyph=1541); gate2 identical counts.
- Embedder surface now: `src/embedder/main.cpp` (init, page setup,
  exported C API: bib_tick/bib_render(force)/bib_frame_*/bib_mouse_*/
  bib_wheel/bib_key/bib_diag/bib_scroll_to),
  `src/embedder/BibPageClients.h` (BibChromeClient dirty flag +
  BibEditorClient typing), `src/embedder/EmbedderStrategies.cpp` (offline
  LoaderStrategy — REPLACE IN THIS PHASE), `web/browser.html`
  (?demo=hello|interactive|scripttest, input forwarding, __bib.probe).
- Host page may inject ANY document via `Module.bibHTML` (UTF-8, copied
  out of JS in main()).
- Three load-bearing Phase 3 root causes (all documented in the memory
  playbook "Interactivity root causes" — READ IT):
  1. pageConfigurationWithEmptyClients → SandboxFlags::all() → script
     silently dead. Cleared via LocalMainFrameCreationParameters in
     interactive mode.
  2. ScrollAnimator::scrollAnimationEnabled only overridden under
     USE(COORDINATED_GRAPHICS); base default TRUE = wheel scrolls
     swallowed by never-ticking animations. Guard-join patch applied.
  3. EmptyChromeClient marks scroll-damage callbacks final — relaxed to
     override in EmptyClients.h (patch ledger).
- WebKit patch ledger ~1500 lines, src/patches/webkit-emscripten.patch.
  Every WebKit edit → tools/export-webkit-patches.sh. NEVER rm -rf
  build/webcore.

## NEXT TASK: Phase 4 — real page loads over Wisp
Goal: `location.href`-style loads of http(s) URLs end-to-end through
WebKit's curl backend → Emscripten sockets → Wisp websocket → wisp server.
1. **Re-enable the curl network stack in WebCore**: PlatformEmscripten.cmake
   currently compiles the curl tier (libcurl.a/OpenSSL in wasm-sysroot are
   ALREADY BUILT + LINKED) but WebCore's network/curl sources are partially
   in the build. Known-missing symbols documented: UserAgent,
   CurlSSLHandle, NetworkStateNotifier. `-Wl,--error-limit=0` is set —
   read the whole list, stub/implement in platform/emscripten/.
2. **WebCrypto/BoringSSL problem comes due** (decision-003): crypto/openssl
   is BoringSSL-flavored (hkdf.h one-shot HKDF, non-const
   EVP_PKEY_get0_EC_KEY, "openssl/X509.h" case). ~4 file patches against
   OpenSSL 3.5 or keep deferred if curl links without it.
3. **Replace EmbedderLoaderStrategy**: the offline strategy returns
   nullptr for every load. Crib the loader flow from WebKitLegacy
   (WebFrameNetworkingContext) or use WebCore's ResourceLoader directly
   with CurlResourceHandle... investigate `loadResource` →
   ResourceLoader::start path on a no-WebKit2 port. SubstituteData loads
   (loadHTMLString) also wanted for the URL bar later.
4. **Wisp socket layer**: Emscripten SOCKFS websocket transport, swap in
   wisp-js `WispWebSocket` (libcurl.js pattern — `Module.websocket.url` +
   subprotocol shim, NO C changes expected). wisp-js server side
   (docs/research has the Wisp report). TLS terminates in-engine via
   OpenSSL; COOP/COEP already served by tools/dev-server.mjs.
5. **Threading decision point**: curl's multi handle normally runs on its
   own thread in WebKit (CurlRequestScheduler). SINGLE-THREADED build —
   either drive curl_multi_perform from the RunLoop (timer pump) or this
   triggers the pthread full-rebuild (~35 min, COOP/COEP already in
   place). Investigate CurlRequestScheduler's thread usage FIRST.
6. Cookies/cache persistence (OPFS) later; data: URLs and images become
   testable once the loader works — add an image to the interactive page
   then.

## Known traps (fresh from Phase 3)
- *Inlines.h: -Wundefined-inline = missing include; the declaring header's
  comment names the file (NodeDocument.h, FrameDestructionObserverInlines.h
  joined DocumentView.h/LocalFrameInlines.h this session).
- New undefined symbols at link: read the FULL wasm-ld list, stub in
  platform/emscripten/, add to PlatformEmscripten.cmake, export patches.
- Empty-client semantics bite silently (sandbox flags, final methods,
  always-true animation defaults). When something "does nothing", check
  what pageConfigurationWithEmptyClients installed before suspecting wasm.
- Scroll animations/KeyboardScrollingAnimator still have NO driver —
  PageDown/arrow-key scrolling of the view likely inert (untested);
  smooth scrolling needs a rAF-driven animation tick (future).
- WTFLogAlways → printErr is the in-engine debugging channel; browser.html
  relays it. bib_diag()/bib_scroll_to() exports exist for state dumps.
- Playwright scripts must live under the project root (node_modules
  resolution); build/ is gitignored and fine for one-offs.

## Build discipline (unchanged)
- tools/build-webcore.sh (resumable, ninja -k 50, font staging, cache
  re-sync). Gates: tools/run-embedder.cjs (node, byte-stable),
  gate2/gate3-browser-test.mjs (dev server:
  `node tools/dev-server.mjs web --mount /engine=build/webcore/bin`).
- One mutator thread per VM. wasm32 only. Local git only. Codex review
  before presenting non-trivial code.
