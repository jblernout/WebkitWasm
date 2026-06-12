/*
 * W-B0 spike (analysis-wb-engine-off-main-thread.md §4) — prove the
 * -pthread + PROXY_TO_PTHREAD shape OUTSIDE WebKit before committing to
 * the full engine rebuild. Throwaway allowed; findings feed W-B1.
 *
 * Questions answered (greppable WBSPIKE: lines):
 *   Q1 SOCKFS WebSocket placement: which JS scope constructs the socket
 *      when C calls connect() on the proxied-main pthread, does the main
 *      page's Module.websocket config propagate there, and can the wisp
 *      dispatcher intercept in that scope (pre-js logs ws-construct).
 *      Acceptance: a real HTTP GET over the wisp shim completes from the
 *      pthread using the engine's poll-yield pattern (emscripten_async_call
 *      chain = the event-driven RunLoop pump shape).
 *   Q2 ALLOW_MEMORY_GROWTH + SHARED_MEMORY: heap must grow well past
 *      INITIAL_MEMORY under shared memory in real browsers.
 *   Q5 stdout ordering pthread->main + abort-stack symbolization across
 *      the worker boundary (?abort=1 mode; --profiling-funcs).
 *   Plus: MAIN_THREAD_EM_ASM proxying, and proof the host main thread
 *   stays responsive while the engine pthread BLOCKS (usleep) — W-B's
 *   entire reason to exist, in miniature.
 *
 * Verdict: "WBSPIKE-VERDICT: PASS ..." / "WBSPIKE-VERDICT: FAIL ...".
 *
 * JS blocks live in EM_JS (not EM_ASM): the C preprocessor splits EM_ASM
 * bodies at top-level commas (object literals), EM_JS re-joins them.
 */

#include <arpa/inet.h>
#include <emscripten.h>
#include <emscripten/em_js.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Worker-scope pre-js lines buffer in self.__wbLog (worker console.log is
 * not reliably visible to the harness); drain them through out() -> proxied
 * Module.print so everything lands on the page console in order. */
EM_JS(void, wb_flush_js_log, (void), {
    var L = self.__wbLog || [];
    for (var i = 0; i < L.length; i++)
        out(L[i]);
    L.length = 0;
});

/* Q2 introspection: is the heap a SharedArrayBuffer, and does it report
 * growable/maxByteLength? (The malloc loop below is the ground truth.) */
EM_JS(int, wb_js_heap_info, (void), {
    var b = HEAPU8.buffer;
    var isSAB = (typeof SharedArrayBuffer !== "undefined") && (b instanceof SharedArrayBuffer);
    out("WBSPIKE: heap-buffer sab=" + isSAB
        + " growable=" + (b.growable !== undefined ? b.growable : "n/a")
        + " maxByteLength=" + (b.maxByteLength !== undefined ? b.maxByteLength : "n/a"));
    return isSAB ? 1 : 0;
});

/* Q3 (informational): OffscreenCanvas + WebGL2 available in the pthread's
 * worker scope? Decides whether GPU mode survives W-B2 per browser. */
EM_JS(void, wb_js_gpu_probe, (void), {
    var scope = (typeof window !== "undefined") ? "main" : "worker";
    var has = (typeof OffscreenCanvas !== "undefined");
    var gl2 = false;
    if (has) {
        try {
            gl2 = !!(new OffscreenCanvas(8, 8).getContext("webgl2"));
        } catch (e) {}
    }
    out("WBSPIKE: gpu-probe scope=" + scope + " offscreenCanvas=" + has + " webgl2=" + gl2);
});

/* Heap size via a REFRESHED view: with growth+pthreads the scope's HEAPU8
 * is stale after another (or even this) thread grows memory — raw
 * HEAPU8.length (what emscripten_get_heap_size's JS reads) underreports
 * until growMemViews() runs. W-B1 blit must re-acquire views per frame. */
EM_JS(double, wb_js_heap_size, (void), {
    if (typeof growMemViews === "function")
        growMemViews();
    return HEAPU8.length;
});

/* Q1 datapoint: Module.websocket lives per-scope. Log whether the page's
 * config reached the scope this pthread executes JS in; install if not. */
EM_JS(void, wb_js_ws_config, (void), {
    var scope = (typeof window !== "undefined") ? "main" : "worker";
    var had = !!Module["websocket"];
    if (!had)
        Module["websocket"] = { url: "ws://", subprotocol: "bib-sockfs" };
    out("WBSPIKE: module-websocket scope=" + scope + " pre-existing=" + had);
});

/* ---- Q5: ?abort=1 mode — does the wasm frame survive to the page? ---- */

__attribute__((noinline)) static void wb_crash_inner(void)
{
    printf("WBSPIKE: aborting now (wb_crash_inner)\n");
    abort();
}

__attribute__((noinline)) static void wb_crash_middle(void)
{
    wb_crash_inner();
}

/* ---- Q1: HTTP GET over the wisp shim, engine-pump style ---- */

static int s_fd = -1;
static int s_port = 8090;
static int s_sent = 0;
static struct sockaddr_in s_sa; /* re-polled connect() needs the real addr */
static char s_resp[65536];
static size_t s_got = 0;
static double s_deadline = 0;
static int s_checksLeft = 0; /* socket check pending at verdict time? */
static int s_failed = 0;

static void wb_verdict(void)
{
    if (s_failed || s_checksLeft)
        printf("WBSPIKE-VERDICT: FAIL failed=%d pending=%d\n", s_failed, s_checksLeft);
    else
        printf("WBSPIKE-VERDICT: PASS\n");
}

static void wb_fail(const char* what)
{
    printf("WBSPIKE: FAIL %s errno=%d\n", what, errno);
    s_failed = 1;
    s_checksLeft = 0;
    wb_flush_js_log();
    wb_verdict();
}

static void wb_poll_socket(void* arg)
{
    (void)arg;
    wb_flush_js_log();
    if (emscripten_get_now() > s_deadline) {
        wb_fail("socket timeout");
        return;
    }

    if (!s_sent) {
        /* SOCKFS connect: EINPROGRESS first, EALREADY while the WebSocket
         * opens, EISCONN once open. Each return to JS lets the worker's
         * event loop deliver the open/message events — the engine pump
         * pattern. */
        int rc = connect(s_fd, (struct sockaddr*)&s_sa, sizeof s_sa);
        if (rc != 0 && errno != EISCONN) {
            if (errno == EINPROGRESS || errno == EALREADY) {
                emscripten_async_call(wb_poll_socket, NULL, 25);
                return;
            }
            wb_fail("connect re-poll");
            return;
        }
        printf("WBSPIKE: socket connected (EISCONN) t=%.0f\n", emscripten_get_now());
        char req[256];
        snprintf(req, sizeof req,
            "GET /wb-spike-hello.txt HTTP/1.1\r\n"
            "Host: 127.0.0.1:%d\r\n"
            "Connection: close\r\n\r\n", s_port);
        ssize_t w = send(s_fd, req, strlen(req), 0);
        if (w != (ssize_t)strlen(req)) {
            wb_fail("send");
            return;
        }
        s_sent = 1;
        emscripten_async_call(wb_poll_socket, NULL, 25);
        return;
    }

    ssize_t r = recv(s_fd, s_resp + s_got, sizeof(s_resp) - 1 - s_got, 0);
    if (r > 0) {
        s_got += (size_t)r;
        s_resp[s_got] = '\0';
    } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        wb_fail("recv");
        return;
    }
    /* Connection: close → server closes after the body; r==0 or the marker
     * already present both mean we can judge. */
    if ((r == 0 && s_got > 0) || strstr(s_resp, "WB-SPIKE-HELLO")) {
        int ok = strstr(s_resp, "HTTP/1.1 200") && strstr(s_resp, "WB-SPIKE-HELLO");
        printf("WBSPIKE: http-over-wisp %s bytes=%zu t=%.0f\n", ok ? "OK" : "BAD", s_got, emscripten_get_now());
        if (!ok)
            s_failed = 1;
        close(s_fd);
        s_checksLeft = 0;
        wb_flush_js_log();
        wb_verdict();
        return;
    }
    if (r == 0) { /* closed with nothing read */
        wb_fail("recv closed empty");
        return;
    }
    emscripten_async_call(wb_poll_socket, NULL, 25);
}

int main(void)
{
    /* Identity: under PROXY_TO_PTHREAD expect runtime-main=1, browser-main=0. */
    printf("WBSPIKE: thread runtime-main=%d browser-main=%d\n",
        emscripten_is_main_runtime_thread(), emscripten_is_main_browser_thread());
    wb_flush_js_log();

    /* MAIN_THREAD_EM_ASM proxy ping + read page config (Module fields on the
     * PAGE's Module — deliberately fetched cross-thread; the worker's Module
     * does not inherit them, which is itself a Q1 datapoint). */
    int ping = MAIN_THREAD_EM_ASM_INT({ return (typeof window !== "undefined") ? 42 : -1; });
    s_port = MAIN_THREAD_EM_ASM_INT({ return Number(Module.wbTargetPort || "8090"); });
    int abortMode = MAIN_THREAD_EM_ASM_INT({ return Module.wbAbort ? 1 : 0; });
    printf("WBSPIKE: main-thread-proxy ping=%d port=%d abortMode=%d\n", ping, s_port, abortMode);
    if (ping != 42)
        s_failed = 1;

    /* Q2: heap growth under shared memory. 16 x 32MB = 512MB, INITIAL=64MB. */
    size_t heap0 = emscripten_get_heap_size();
    int sab = wb_js_heap_info();
    /* Chunks must be observable or LLVM elides the whole malloc+memset loop
     * (dead-allocation elimination — verified: -O1 deleted it). */
    static unsigned char* chunks[16];
    unsigned sum = 0;
    int growOK = 1;
    for (int i = 0; i < 16; i++) {
        chunks[i] = (unsigned char*)malloc(32u * 1024 * 1024);
        if (!chunks[i]) {
            printf("WBSPIKE: malloc chunk=%d FAILED heap=%zuMB errno=%d\n",
                i, emscripten_get_heap_size() >> 20, errno);
            growOK = 0;
            break;
        }
        memset(chunks[i], 0xbb, 32u * 1024 * 1024); /* touch every page; leak on purpose */
        sum += chunks[i][12345];
    }
    printf("WBSPIKE: malloc sum=%u (expect %u)\n", sum, growOK ? 16u * 0xbb : 0u);
    size_t heap1 = (size_t)wb_js_heap_size();
    size_t heap1stale = emscripten_get_heap_size();
    printf("WBSPIKE: mem-growth %s sab=%d heap %zuMB -> %zuMB (stale-view reads %zuMB)\n",
        (growOK && heap1 > heap0) ? "OK" : "FAIL", sab,
        heap0 >> 20, heap1 >> 20, heap1stale >> 20);
    if (!growOK || heap1 <= heap0 || !sab)
        s_failed = 1;

    if (abortMode) {
        wb_crash_middle(); /* Q5: never returns */
        return 0;
    }

    /* Blocking demo: the engine pthread may legally sleep; the page ticker
     * must keep ticking between block-begin/block-end (runner checks). */
    printf("WBSPIKE: block-begin t=%.0f\n", emscripten_get_now());
    usleep(1200 * 1000);
    printf("WBSPIKE: block-end t=%.0f\n", emscripten_get_now());

    wb_js_gpu_probe(); /* Q3, runs in this pthread's JS scope */

    /* Q1: socket through the wisp shim. */
    wb_js_ws_config();

    s_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_fd < 0) {
        wb_fail("socket()");
        return 0;
    }
    memset(&s_sa, 0, sizeof s_sa);
    s_sa.sin_family = AF_INET;
    s_sa.sin_port = htons((uint16_t)s_port);
    inet_pton(AF_INET, "127.0.0.1", &s_sa.sin_addr);
    int rc = connect(s_fd, (struct sockaddr*)&s_sa, sizeof s_sa);
    if (rc != 0 && errno != EINPROGRESS) {
        wb_fail("connect()");
        return 0;
    }
    printf("WBSPIKE: connect initiated rc=%d errno=%d t=%.0f\n", rc, errno, emscripten_get_now());
    wb_flush_js_log();

    s_checksLeft = 1;
    s_deadline = emscripten_get_now() + 20000;
    emscripten_async_call(wb_poll_socket, NULL, 25);
    /* Return with the runtime alive (EXIT_RUNTIME=0): the pthread's event
     * loop must turn for async_call + WebSocket events — exactly how the
     * engine RunLoop pump will live on this thread under W-B1. */
    return 0;
}
