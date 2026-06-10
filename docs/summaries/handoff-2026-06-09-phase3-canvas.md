# Handoff — Phase 3: canvas blit + interactivity (the ONE active handoff)

**Written**: 2026-06-09 ~23:50 EDT, minutes after the Phase 2 gate passed.
**Supersedes**: handoff-2026-06-09-phase2-embedder.md (→ docs/archive/).

## State (commits a680190 → da01669; Codex review of embedder IN FLIGHT)
- **PHASE 2 GATE PASSED**: WebKit-in-wasm painted `<h1>hello</h1>` + a
  styled div, 800×600, pixel-exact, under node. Proof: `build/out.png`.
  Full stack live: HTML parser → style → layout → fontconfig/FreeType/
  HarfBuzz shaping → Skia CPU raster → PPM. Render tree dump shows real
  font metrics (`text run width 116`).
- `embedder.wasm` 87.6 MB = WebCore+JSC+WTF+PAL+Skia+curl tier, with
  `--embed-file` ICU dat + `/etc/fonts` + DejaVuSans.
- Embedder code: `src/embedder/main.cpp` (Page construction → DocumentWriter
  load → layout → GraphicsContextSkia paint → `/out.ppm`),
  `src/embedder/EmbedderStrategies.cpp` (offline LoaderStrategy — REQUIRED:
  FrameLoader::pageLoadCompleted dereferences it), `src/embedder/embedder.cmake`
  (deferred-include target in WebCore's CMake scope; WebCoreTestSupport
  include-dir pattern). Runner: `tools/run-embedder.cjs` (post-require
  Module config — global.Module is shadowed under CJS).
- WebKit tree delta: 12 stub files (AX ×2, WebCrypto, Editor, Pasteboard,
  MIME, keyboard, screen, scrollbar/theme/render-theme/font-db singletons,
  GL link stubs) + 2 guard joins (AXCoreObject.h, Pasteboard.h) + the
  3-line embedder hook in PlatformEmscripten.cmake. Ledger ~1500 lines.
- Everything SINGLE-THREADED (no -pthread in any object). Deliberate.

## NEXT TASK: Phase 3 — pixels in a real tab, then input
1. **Canvas blit**: host page (extend tools/dev-server.mjs assets) loads
   embedder.js in a COOP/COEP tab (Phase 1's gate1-browser-test.mjs is the
   Playwright template). Replace the PPM dump with an exported
   `render(ptr)` path: either main() keeps the surface alive + a
   `EMSCRIPTEN_KEEPALIVE` function returns the pixel pointer, or switch
   the embedder to `-sMODULARIZE` and drive it from JS. Blit via
   `ctx.putImageData(new ImageData(new Uint8ClampedArray(HEAPU8.buffer,
   ptr, w*h*4), w, h), 0, 0)`. Browser fonts/ICU already embedded —
   nothing else to package.
2. **A real RunLoop**: main() currently exits after one paint. For a live
   page: after load, enter the generic RunLoop (RunLoop::run()) and drive
   paints from a requestAnimationFrame→C callback (emscripten_set_main_loop
   conflicts with RunLoop; prefer exported tick function called from rAF
   that runs RunLoop::cycle() + paint-if-dirty via ChromeClient invalidation
   — override EmptyChromeClient::invalidateContentsAndRootView).
3. **Input forwarding**: canvas mouse/key events → exported C functions →
   `EventHandler::handleMousePressEvent(PlatformMouseEvent)` etc. Crib
   event construction from any port's WebView (PlayStation/WinCairo).
4. **Script back on**: setScriptEnabled(true) — JSC inside a WebCore page
   is UNTESTED in this port (CLoop on the main thread; watch for timer/
   microtask starvation without a live RunLoop — do this AFTER step 2).
5. Then images (decoders are compiled: png/jpeg/webp), scrolling, more CSS.

## Known traps for Phase 3 (from tonight)
- `-Wundefined-inline` errors = missing 2.52 `*Inlines.h` include; the
  declaring header's comment names the right file (e.g. DocumentView.h).
- New undefined symbols at link will appear as features light up
  (UserAgent, CurlSSLHandle, NetworkStateNotifier are known-missing for
  Phase 4). `-Wl,--error-limit=0` is already set — read the WHOLE list.
- Runtime untested beyond one paint: Thread::create anywhere = trap
  (single-threaded build). If something needs a thread, that's the
  pthread-rebuild decision point (~35 min full rebuild, browser side
  needs the COOP/COEP headers we already serve).
- fontconfig cache dir /var/cache/fontconfig is created by the RUNNER
  (preRun) — the browser host page must do the same FS.mkdirTree.
- node runner gotchas are documented in tools/run-embedder.cjs header.

## Build discipline (unchanged)
- NEVER `rm -rf build/webcore` (~35 min full vs 2–8 min incremental).
- Every WebKit edit → `tools/export-webkit-patches.sh`.
- One mutator thread per VM. ENABLE_SAMPLING_PROFILER OFF. wasm32 only.
- Local git only. Codex review before presenting non-trivial code.
- Memory playbook: webkit-wasm-porting-playbook ("Embedder against
  internal headers" + "Port-stub surface" sections are tonight's recipe).
