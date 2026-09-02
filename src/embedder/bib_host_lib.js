// JS side of src/embedder/bib_host.h, linked with --js-library.
//
// Each function is what the former EM_ASM block at the call site did, against
// the same Module.* contract web/browser.html and web/engine-pre.js implement.
// __proxy carries the threading semantics of the old macros: 'sync' for
// MAIN_THREAD_EM_ASM (block the engine pthread until the browser main thread
// answers), 'async' for MAIN_THREAD_ASYNC_EM_ASM (post to the main thread),
// none for plain EM_ASM (run in the calling thread's scope — the worker
// hooks of engine-pre.js). In a non-pthread build __proxy is inert.
//
// NOTE: cmake does not track edits to this file — touch main.cpp to relink.

addToLibrary({
  // --- boot flags -----------------------------------------------------------
  bib_host_flag__deps: ['$UTF8ToString'],
  bib_host_flag__proxy: 'sync',
  bib_host_flag__sig: 'ip',
  bib_host_flag: (namePtr) => {
    var M = (typeof Module !== "undefined" && Module) ? Module : {};
    switch (UTF8ToString(namePtr)) {
      case "width": return (M.bibWidth | 0) || 0;
      case "height": return (M.bibHeight | 0) || 0;
      case "interactive": return M.bibInteractive ? 1 : 0;
      case "gpu": return M.bibGPU ? 1 : 0;
      case "canvasgpu": return M.bibCanvasGPU ? 1 : 0;
      case "gpubench": return M.bibGpuBench === false ? 0 : 1;
      case "media": return M.bibMedia ? 1 : 0;
      case "curldebug": return M.bibCurlDebug ? 1 : 0;
      case "noblock": return M.bibNoBlock ? 1 : 0;
      case "perflog":
        try { return new URLSearchParams(location.search).get("perflog") === "1" ? 1 : 0; }
        catch (e) { return 0; }
      case "gclog":
        try { return new URLSearchParams(location.search).get("gclog") === "1" ? 1 : 0; }
        catch (e) { return 0; }
      case "rcap":
        try {
          var s = new URLSearchParams(location.search).get("rcap");
          if (s === null) return -1; // absent => dynamic
          var v = parseInt(s, 10);
          return (v >= 0 && v <= 240) ? v : -1;
        } catch (e) { return -1; }
    }
    return 0;
  },

  // --- host strings ---------------------------------------------------------
  bib_host_string__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  bib_host_string__proxy: 'sync',
  bib_host_string__sig: 'pp',
  bib_host_string: (namePtr) => {
    var M = (typeof Module !== "undefined" && Module) ? Module : {};
    var text = null;
    switch (UTF8ToString(namePtr)) {
      case "html": text = M.bibHTML; break;
      case "seedstate": text = M.bibSeedState; break;
      case "wasmpolyfill": text = M.bibWasmPolyfill; break;
    }
    if (typeof text !== "string" || !text.length)
      return 0;
    var len = lengthBytesUTF8(text) + 1;
    var buf = _bib_wasm_alloc(len);
    if (!buf)
      return 0;
    stringToUTF8(text, buf, len);
    return buf;
  },

  // --- pump (calling-thread scope) ------------------------------------------
  bib_host_wakeup__sig: 'v',
  bib_host_wakeup: () => { if (Module.bibWakeUp) Module.bibWakeUp(); },
  bib_host_arm_timer__sig: 'vd',
  bib_host_arm_timer: (ms) => { if (Module.bibArmTimer) Module.bibArmTimer(ms); },
  bib_host_install_worker_hooks__sig: 'v',
  bib_host_install_worker_hooks: () => {
    if (typeof self !== "undefined" && self.__bibInstallWorkerHooks)
      self.__bibInstallWorkerHooks();
  },

  // --- frame / blob pushes (main thread, async) -----------------------------
  // growMemViews() first: views go stale after a cross-thread memory grow.
  bib_host_blit__proxy: 'async',
  bib_host_blit__sig: 'vpiiii',
  bib_host_blit: (ptr, x, y, w, h) => {
    if (typeof growMemViews === "function") growMemViews();
    var bytes = HEAPU8.slice(ptr, ptr + w * h * 4);
    _bib_wasm_free(ptr);
    if (Module.bibBlit) Module.bibBlit(bytes, x, y, w, h);
  },
  bib_host_readback_ready__proxy: 'async',
  bib_host_readback_ready__sig: 'vpiii',
  bib_host_readback_ready: (ptr, bytes, w, h) => {
    var data = null;
    if (ptr) {
      if (typeof growMemViews === "function") growMemViews();
      data = HEAPU8.slice(ptr, ptr + bytes);
      _bib_wasm_free(ptr);
    }
    if (Module.bibReadbackReady) Module.bibReadbackReady(data, w, h);
  },
  bib_host_persist__deps: ['$UTF8ToString'],
  bib_host_persist__proxy: 'async',
  bib_host_persist__sig: 'vp',
  bib_host_persist: (ptr) => {
    if (typeof growMemViews === "function") growMemViews();
    var json = UTF8ToString(ptr);
    _bib_wasm_free(ptr);
    // pagehide nulls window.Module while a push may be in flight: free-then-drop.
    if (typeof Module !== "undefined" && Module && Module.bibPersist) Module.bibPersist(json);
  },
  bib_host_ready__proxy: 'async',
  bib_host_ready__sig: 'v',
  bib_host_ready: () => { if (Module.onEngineReady) Module.onEngineReady(); },

  // --- GPU -----------------------------------------------------------------
  bib_host_gpu_event__proxy: 'async',
  bib_host_gpu_event__sig: 'vi',
  bib_host_gpu_event: (kind) => {
    if (kind === 1 && Module.bibGpuFallback) Module.bibGpuFallback();
    if (kind === 2 && Module.bibGpuLostReload) Module.bibGpuLostReload();
  },
  bib_host_present_ready__sig: 'i',
  bib_host_present_ready: () => (Module.bibBitmapPresentReady && Module.bibBitmapPresentReady()) ? 1 : 0,
  bib_host_present_transfer__sig: 'i',
  bib_host_present_transfer: () => (Module.bibTransferCurrentFrame && Module.bibTransferCurrentFrame() === 1) ? 1 : 0,
  bib_host_present_hello__sig: 'vii',
  bib_host_present_hello: (w, h) => { if (Module.bibPresentWorkerHello) Module.bibPresentWorkerHello(w, h); },

  // --- wasm2js -----------------------------------------------------------------
  bib_host_wasm2js__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  bib_host_wasm2js__sig: 'ppp',
  bib_host_wasm2js: (payloadPtr, modePtr) => {
    var result = null;
    try {
      if (Module.bibWasm2js)
        result = Module.bibWasm2js(UTF8ToString(payloadPtr), UTF8ToString(modePtr));
    } catch (e) {
      (Module.printErr || console.error)("bibWasm2js host error: " + e);
    }
    if (typeof result !== "string")
      return 0;
    var len = lengthBytesUTF8(result) + 1;
    // A multi-MB translated module can fail to allocate under heap pressure;
    // stringToUTF8 through a null pointer would trap.
    var buf = _bib_wasm_alloc(len);
    if (!buf)
      return 0;
    stringToUTF8(result, buf, len);
    return buf;
  },

  // --- network idle: browsers use Page.lifecycleEvent / IdlenessDetector -------
  bib_host_net_inflight__sig: 'vi',
  bib_host_net_inflight: (delta) => {},

  // --- resource cache: the browser's own HTTP cache does this job -----------------
  bib_host_cache_get__sig: 'ipppppp',
  bib_host_cache_get: (url, status, headers, headersLen, body, bodyLen) => 0,
  bib_host_cache_put__sig: 'vpipipi',
  bib_host_cache_put: (url, status, headers, headersLen, body, bodyLen) => {},

  // --- media bridge -------------------------------------------------------------
  bib_host_media_can_play__deps: ['$UTF8ToString'],
  bib_host_media_can_play__proxy: 'sync',
  bib_host_media_can_play__sig: 'ip',
  bib_host_media_can_play: (typePtr) =>
    (typeof Module !== "undefined" && Module && Module.bibMediaCanPlay)
      ? Module.bibMediaCanPlay(UTF8ToString(typePtr)) : 0,
  bib_host_media_create__proxy: 'async',
  bib_host_media_create__sig: 'vi',
  bib_host_media_create: (id) => { if (Module.bibMediaCreate) Module.bibMediaCreate(id); },
  bib_host_media_load_bytes__deps: ['$UTF8ToString'],
  bib_host_media_load_bytes__proxy: 'async',
  bib_host_media_load_bytes__sig: 'vipip',
  bib_host_media_load_bytes: (id, ptr, len, mimePtr) => {
    if (typeof growMemViews === "function") growMemViews();
    var bytes = HEAPU8.slice(ptr, ptr + len);
    var mime = UTF8ToString(mimePtr);
    _bib_wasm_free(ptr);
    _bib_wasm_free(mimePtr);
    if (typeof Module !== "undefined" && Module && Module.bibMediaLoadBytes)
      Module.bibMediaLoadBytes(id, bytes, mime);
  },
  bib_host_media_destroy__proxy: 'async',
  bib_host_media_destroy__sig: 'vi',
  bib_host_media_destroy: (id) => { if (Module.bibMediaDestroy) Module.bibMediaDestroy(id); },
  bib_host_media_ctl__proxy: 'async',
  bib_host_media_ctl__sig: 'viid',
  bib_host_media_ctl: (id, op, a) => { if (Module.bibMediaCtl) Module.bibMediaCtl(id, op, a); },
});
