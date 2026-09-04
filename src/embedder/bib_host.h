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

// Resource cache, host-owned and shared across page loads and engines.
// bib_host_cache_get: for a GET, a hit fills *status, *headers (a
// bib_wasm_alloc'd "Name: value\r\n" block of *headersLen bytes), *body
// (bib_wasm_alloc'd, *bodyLen bytes) and *fresh, and returns 1; a miss
// returns 0 with nothing allocated. A stale hit (*fresh == 0) must be
// revalidated: send the request with If-None-Match / If-Modified-Since taken
// from the cached headers and, on 304, call bib_host_cache_touch. The caller
// frees both buffers.
int bib_host_cache_get(const char* url, int* status, char** headers, int* headersLen, uint8_t** body, int* bodyLen, int* fresh);
// bib_host_cache_put offers a complete response (raw header lines, decoded
// body); the host decides cacheability from the headers and copies out.
void bib_host_cache_put(const char* url, int status, const char* headers, int headersLen, const uint8_t* body, int bodyLen);
// bib_host_cache_touch reports a 304 for a stale entry: the host merges the
// 304's headers into the entry, extends its lifetime and hands back the
// merged header block (bib_wasm_alloc'd) to deliver with the cached body;
// returns 0 when the entry is gone (load the resource normally).
int bib_host_cache_touch(const char* url, const char* headers304, int headers304Len, char** headers, int* headersLen);

// Network idle detection: +1 when a resource load starts, -1 when it ends
// (success, failure or cancel) — main document, subresources, beacons and
// host-cache hits alike, so the host can apply Chrome's "no request in
// flight for N ms" rule.
void bib_host_net_inflight(int delta);

// Host HTTP transport. When bib_host_flag("hostfetch") is set, resource loads
// are performed by the host (which can present a browser TLS / HTTP2
// fingerprint) instead of the engine's curl: bib_host_fetch starts request
// `id` (header block "Name: value\r\n", body bytes) and returns 1 when the
// host took it, 0 to fall back to curl. The host answers through exports, on
// the engine thread and in order: bib_fetch_head(id, status, headers, len)
// once, bib_fetch_data(id, bytes, len) per body chunk as it arrives, then
// bib_fetch_done(id, errno) (errno != 0: transport failure, possibly before
// any head). Buffers are bib_wasm_alloc'd by the host and freed by the
// engine. Redirects are not followed by the host (3xx come back as is);
// bib_host_fetch_cancel stops a stream the engine no longer wants.
int bib_host_fetch(int id, const char* method, const char* url, const char* headers, int headersLen, const uint8_t* body, int bodyLen);
void bib_host_fetch_cancel(int id);

// Request policy. Called before every subresource / ping load with the URL,
// the resource kind ("main" for frame documents, "image", "json", "css",
// "script", "font", "media", "raw" = XHR/fetch/EventSource, "icon", "beacon",
// "ping", "prefetch", "other") and whether the requesting frame is the top
// frame. 1 = load (the built-in blocklist is skipped), 0 = refuse (the load
// fails before starting, like a blocklisted host), -1 = no opinion: the
// built-in blocklist decides. Browsers have no host policy: -1.
int bib_host_allow_request(const char* url, const char* type, int mainFrame);

// Memory: mimalloc (BIB_MALLOC=mimalloc) hands the host the ranges of linear
// memory it decommits; the host drops their physical pages (they read as
// zero afterwards). Browsers have no such control: no-op.
void bib_host_discard(void* addr, size_t size);
// Same, but never queued (mimalloc memory returned to emmalloc, which may
// reuse it before the queue is flushed).
void bib_host_discard_now(void* addr, size_t size);
// on=1: queue the following bib_host_discard calls; on=0: drop the merged
// ranges now. Only around code that cannot allocate (mi_collect).
void bib_host_discard_batch(int on);

// JavaScript bytecode cache (bib_host_flag("jsbytecode")): WebCore's script
// source provider stores JSC's serialized unlinked bytecode per script
// (key = source hash, length, URL) and asks for it before parsing. get()
// returns a malloc'd blob (freed by the engine) or NULL; the engine commits
// through bib_flush_bytecode() (bib_reset calls it) and at provider teardown.
char* bib_host_bytecode_get(const char* key, int* outLen);
void bib_host_bytecode_put(const char* key, const uint8_t* data, int len);

// Media bridge (BibMediaPlayer): host-side <audio>/<video> elements.
int bib_host_media_can_play(const char* contentType);
void bib_host_media_create(int id);
// The host frees bytes and mime.
void bib_host_media_load_bytes(int id, uint8_t* bytes, int len, char* mime);
void bib_host_media_destroy(int id);
void bib_host_media_ctl(int id, int op, double a);

} // extern "C"
