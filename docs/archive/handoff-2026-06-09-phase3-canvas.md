# Handoff — Phase 3: canvas blit + interactivity (the ONE active handoff)

**Written**: 2026-06-09 ~23:50 EDT, minutes after the Phase 2 gate passed.
**Finalized**: ~23:57 after the Codex review (0 crit / 0 high / 2 med /
3 low — ALL FIXED in c8fbf59; gate re-verified with strict assertions:
exactBlue=20000, redGlyph=1541).
**Updated 2026-06-10 ~00:10**: STEP 1 (canvas blit) DONE — GATE2-BROWSER
PASS, identical pixel counts in a real Chromium tab, 11 sustained rAF
frames. See "Step 1 outcome" below; next is step 2/3 (invalidation-driven
repaint, then input).
**Supersedes**: handoff-2026-06-09-phase2-embedder.md (→ docs/archive/).

## State (commits a680190 → c8fbf59, tree clean, review done)
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

## Step 1 outcome (DONE 2026-06-10 ~00:09)
- `src/embedder/main.cpp` restructured: engine state in an intentionally
  leaked global `Engine` struct; gate mode unchanged (node PPM path still
  passes byte-identical); `Module.bibInteractive` (read via EM_ASM_INT)
  switches to interactive mode → `Module.onEngineReady()` then
  `emscripten_exit_with_live_runtime()`.
- Exported C API: `bib_frame_width/height`, `bib_tick()`
  (WTF::RunLoop::cycle() — RunMode::Iterate NEVER blocks, verified in
  RunLoopGeneric.cpp: the waitUntil is Drain-only), `bib_render()`
  (repaint + readPixels into static unpremul RGBA buffer).
- `web/browser.html` host page: pre-set window.Module (browser var-Module
  pickup works, unlike node CJS), preRun mkdirTree /var/cache/fontconfig,
  rAF loop _bib_tick→_bib_render→putImageData with a FRESH heap view each
  frame (ALLOW_MEMORY_GROWTH detaches). Verdict computed from the engine's
  own bytes, NOT canvas getImageData (premul round-trip is lossy).
- `tools/dev-server.mjs`: `--mount /engine=build/webcore/bin` (realpath
  containment per mount root) — no copying 87 MB artifacts into web/.
- `tools/gate2-browser-test.mjs`: Playwright gate. PASS = identical counts
  (exactBlue=20000, redGlyph=1541) + frames>=10 liveness.
  Run: `node tools/dev-server.mjs web --mount /engine=build/webcore/bin`
  then `node tools/gate2-browser-test.mjs`. Proof: build/gate2-canvas.png.
- `embedder.cmake`: EXPORTED_RUNTIME_METHODS=FS,HEAPU8.

## NEXT TASK: Phase 3 remaining — invalidation, input, script
2. **Invalidation-driven repaint**: today every rAF frame repaints fully
   (intentional for the milestone). Override
   EmptyChromeClient::invalidateContentsAndRootView (custom ChromeClient in
   the PageConfiguration — pageConfigurationWithEmptyClients then replace,
   or build the config by hand) to set a dirty flag/rect; bib_render()
   becomes paint-if-dirty (still must repaint on first frame).
3. **Input forwarding**: canvas mouse/key events → exported C functions →
   `EventHandler::handleMousePressEvent(PlatformMouseEvent)` etc. Crib
   event construction from any port's WebView (PlayStation/WinCairo).
   Hover/`:hover` restyle is the visual proof (needs step 2's invalidation
   to show up).
4. **Script back on**: setScriptEnabled(true) — JSC inside a WebCore page
   is UNTESTED in this port (CLoop on the main thread). bib_tick() already
   cycles the RunLoop every frame, so timers/microtasks have a heartbeat;
   watch for starvation between rAF ticks.
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
