// Host hooks of the BrowserInBrowser embedder.
//
// Every place the engine used to reach its host through an EM_ASM block that
// read the page's Module now goes through one of these C imports. The wasm
// import section therefore names each hook: a non-JS host (the Go/wazero host)
// implements them by name instead of pattern-matching JS source, and the JS
// host keeps the same Module.* contract through bib_host_lib.js (linked with
// --js-library), which also carries the main-thread proxying the former
// MAIN_THREAD_EM_ASM / MAIN_THREAD_ASYNC_EM_ASM sites had.
//
// Conventions:
//   * strings passed in are NUL-terminated UTF-8 in the engine heap;
//   * strings returned are allocated with bib_wasm_alloc and owned by the
//     caller (release with free());
//   * buffers pushed to the host (blit, readback, persist, media bytes) are
//     malloc'd by the engine; the host copies them out and releases them with
//     bib_wasm_free (dlmalloc is thread-safe under -pthread).
#pragma once

#include <stdint.h>

extern "C" {

// Boot flags the host exposes (Module.bib<Name> in JS); 0 when unset.
//   "width" "height"      viewport (0 = engine default)
//   "interactive"         host drives a tick loop (vs one-shot gate render)
//   "gpu" "canvasgpu"     Skia GPU boot / guest canvases on the GPU
//   "gpubench"            enforce the software-renderer bench (default 1)
//   "media"               media bridge
//   "curldebug"           libcurl verbose tracing
//   "noblock"             disable the request blocklist
//   "perflog" "gclog"     diagnostics (?perflog=1 / ?gclog=1 on the host URL)
//   "rcap"                rendering-update cap: -1 absent, 0 off, 1..240 fixed
int bib_host_flag(const char* name);

// Host-provided text: "html" (initial document), "seedstate" (persistence
// seed JSON), "wasmpolyfill" (guest injection text). bib_wasm_alloc'd copy,
// or nullptr when the host has none.
char* bib_host_string(const char* name);

// Event-driven pump. These run on the calling thread (never proxied): the
// worker-scope hooks of web/engine-pre.js schedule on the engine thread.
void bib_host_wakeup(void);
void bib_host_arm_timer(double ms);
void bib_host_install_worker_hooks(void);

// Raster frame push: w*h*4 RGBA bytes at ptr for the dirty box (x, y, w, h).
// The host frees ptr.
void bib_host_blit(uint8_t* ptr, int x, int y, int w, int h);
// Full-frame readback push (ptr may be null when the render failed). The
// host frees ptr.
void bib_host_readback_ready(uint8_t* ptr, int bytes, int w, int h);
// Persistence blob push (NUL-terminated JSON). The host frees json.
void bib_host_persist(char* json);
// The engine is ready for the host's tick loop (Module.onEngineReady).
void bib_host_ready(void);

// GPU host events: 1 = the engine fell back to raster (host must stop
// expecting GPU frames), 2 = the GL context is gone for good (host reload).
void bib_host_gpu_event(int kind);
// Zero-copy GPU present handshake (pthread build): backpressure gate,
// transfer of the current frame (1 = posted), and the port handshake.
int bib_host_present_ready(void);
int bib_host_present_transfer(void);
void bib_host_present_hello(int w, int h);

// wasm2js translation service for guest WebAssembly: bib_wasm_alloc'd JS
// source, or nullptr when the host has none.
char* bib_host_wasm2js(const char* payload, const char* mode);

// Media bridge (BibMediaPlayer): host-side <audio>/<video> elements.
int bib_host_media_can_play(const char* contentType);
void bib_host_media_create(int id);
// The host frees bytes and mime.
void bib_host_media_load_bytes(int id, uint8_t* bytes, int len, char* mime);
void bib_host_media_destroy(int id);
void bib_host_media_ctl(int id, int op, double a);

} // extern "C"
