# GPT-5.5 Pro design — zero-copy GPU present (verbatim)

Captured 2026-06-13. Conversation: https://chatgpt.com/c/6a2cef6b-4740-83ea-b438-2178c46c5c90
Prompt sent: /tmp/gpt55-prompt.txt (this session). Attached: main.cpp, PlatformDisplayEmscripten.cpp,
browser.html, engine-pre.js, embedder.cmake, EmbedderStrategies.cpp.
This is GPT-5.5's answer VERBATIM — crosscheck before trusting (see handoff for my assessment).

---

WORKER->MAIN ImageBitmap plumbing

Use a dedicated MessageChannel. Do not use the pthread Worker’s raw self.postMessage stream for frame traffic. The raw Worker message stream is owned by Emscripten’s pthread runtime. Use it only for a tiny custom “hello” and for transferring one MessagePort into the engine worker. After that, all frame traffic goes over the dedicated port.

The attached engine-pre.js already exists because, under PROXY_TO_PTHREAD, EM_ASM executes in the engine pthread worker scope and that worker’s Module does not inherit page-side Module fields. It also already installs worker-local MessageChannel pump hooks from C via self.__bibInstallWorkerHooks(). Reuse that pattern. The C main() already calls self.__bibInstallWorkerHooks() early in the worker, before the engine starts driving WebCore. 

engine-pre

 

main

A. engine-pre.js worker-side addition

Add this inside installHooks(), after the existing pump hook setup and before return true.

JavaScript
// --- GPU bitmap present bridge -----------------------------------------
// Runs only in the engine pthread worker. The main page will discover this
// worker via Module.bibPresentWorkerHello(), then transfer a MessagePort to
// us. All ImageBitmap frames use that port, not Emscripten's pthread control
// pipe.

Module.__bibPresent = {
  port: null,
  ready: false,       // main has consumed previous ImageBitmap
  nextId: 0,
  width: 0,
  height: 0,
  framesPosted: 0,
  framesSkipped: 0
};

function installBibPresentPort(port) {
  if (!port)
    return;

  Module.__bibPresent.port = port;
  Module.__bibPresent.ready = false;

  port.onmessage = function (e) {
    var d = e.data || {};

    if (d.t === "ready") {
      // One-in-flight backpressure release.
      Module.__bibPresent.ready = true;
      return;
    }

    if (d.t === "close") {
      Module.__bibPresent.ready = false;
      Module.__bibPresent.port = null;
      return;
    }
  };

  if (port.start)
    port.start();

  // Allow first frame immediately after the port arrives. Main also sends
  // "ready"; this makes the protocol robust against ordering edits.
  Module.__bibPresent.ready = true;
}

// This receives the port transferred by browser.html. The message is sent
// to the Emscripten pthread Worker object, so Emscripten's own onmessage will
// also see it. Keep the shape custom and do not use a "cmd" field.
self.addEventListener("message", function (e) {
  var d = e.data || {};
  if (!d.__bibPresentPort)
    return;

  installBibPresentPort(d.port || (e.ports && e.ports[0]));
});

// Called from C++ once main() has installed worker hooks. This is the only
// raw self.postMessage() we use. It lets browser.html identify which
// Emscripten-created Worker is the proxied-main/engine pthread.
//
// Important: no "cmd" field. Do not collide with Emscripten pthread protocol.
Module.bibPresentWorkerHello = function (w, h) {
  Module.__bibPresent.width = w | 0;
  Module.__bibPresent.height = h | 0;

  self.postMessage({
    __bibPresent: "hello",
    w: Module.__bibPresent.width,
    h: Module.__bibPresent.height
  });
};

// C++ asks this before it snapshots/clears WebCore damage. If false, C++
// returns without painting; damage remains armed and coalesces into a later
// frame.
Module.bibBitmapPresentReady = function () {
  var p = Module.__bibPresent;
  return !!(p && p.port && p.ready);
};

// C++ calls this only after:
//   1. WebCore/Ganesh has painted into the persistent texture SkSurface.
//   2. That texture SkSurface has been drawn into the OffscreenCanvas FBO0.
//   3. skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get()) has run.
//
// transferToImageBitmap() captures the current OffscreenCanvas bitmap and
// replaces the OffscreenCanvas' image with a new blank one for subsequent
// drawing. That is okay because the persistent browser-frame backing store
// is g_engine->surface, not FBO0.
Module.bibTransferCurrentFrame = function () {
  var p = Module.__bibPresent;
  if (!p || !p.port || !p.ready) {
    if (p) p.framesSkipped++;
    return 0; // not ready; C++ should not have called this after ready check
  }

  var entry = typeof GL !== "undefined" &&
    GL.offscreenCanvases &&
    GL.offscreenCanvases["bibgpu"];

  var oc = entry && (entry.offscreenCanvas || entry.canvas);
  if (!oc || typeof oc.transferToImageBitmap !== "function")
    return -1;

  var bitmap;
  try {
    bitmap = oc.transferToImageBitmap();
  } catch (e) {
    console.error("BIB: transferToImageBitmap failed: " + e);
    return -2;
  }

  p.ready = false;
  var id = ++p.nextId;
  p.framesPosted++;

  // Zero-copy ownership transfer of the ImageBitmap to the page main thread.
  p.port.postMessage({
    t: "frame",
    id: id,
    bitmap: bitmap,
    w: p.width,
    h: p.height
  }, [bitmap]);

  return 1;
};

B. C++ one-time worker hello

Add this immediately after the existing EM_ASM that calls self.__bibInstallWorkerHooks() in main().

C++
EM_ASM({
    if (typeof self !== "undefined" && self.__bibInstallWorkerHooks)
        self.__bibInstallWorkerHooks();
});

#ifdef __EMSCRIPTEN_PTHREADS__
EM_ASM({
    if (Module.bibPresentWorkerHello)
        Module.bibPresentWorkerHello($0, $1);
}, kWidth, kHeight);
#endif

That makes the engine pthread identify itself to the page without needing Module.PThread.runningWorkers or any Emscripten internal worker registry.

C. browser.html main-side present-mode setup

Replace the current unconditional 2D context acquisition:

JavaScript
const ctx = canvas.getContext("2d");

The current browser.html always grabs a 2D context, even in GPU mode, because Approach R routes GPU output back through bibBlit/putImageData. That must change because a canvas context type is exclusive: if you call getContext("2d"), you cannot later use bitmaprenderer or webgl2 on that same canvas. 

browser

Use this early, before loading engine/embedder.js.

JavaScript
const canvas = document.getElementById("screen");

const params = new URLSearchParams(location.search);
const gpuParam = params.get("gpu");

// Same default policy as today: GPU for humans, raster for automation unless
// overridden.
const requestedGPU = gpuParam !== null ? gpuParam !== "0" : !navigator.webdriver;

// Stamp this at packaging time. Do not detect it from runtime behavior after
// getContext(), because context acquisition is one-way.
const BIB_PTHREAD_BUILD = true; // set false in the BIB_PTHREAD=OFF build

function supportsBitmapPresent() {
  if (typeof OffscreenCanvas !== "function")
    return false;
  try {
    const c = document.createElement("canvas");
    return !!c.getContext("bitmaprenderer");
  } catch (e) {
    return false;
  }
}

const gpuMode =
  requestedGPU &&
  (!BIB_PTHREAD_BUILD || supportsBitmapPresent());

const presentMode =
  !gpuMode ? "raster-2d" :
  BIB_PTHREAD_BUILD ? "gpu-bitmap" :
  "gpu-implicit";

let ctx = null;        // existing raster/putImageData path uses this
let bitmapCtx = null;  // pthread GPU bitmap path uses this

if (presentMode === "raster-2d") {
  ctx = canvas.getContext("2d");
  if (!ctx)
    throw new Error("BIB: failed to create 2d context");
} else if (presentMode === "gpu-bitmap") {
  bitmapCtx = canvas.getContext("bitmaprenderer");
  if (!bitmapCtx)
    throw new Error("BIB: failed to create bitmaprenderer context");
} else {
  // gpu-implicit: do not call getContext() here. The C++ engine will call
  // emscripten_webgl_create_context("#screen") and own the WebGL2 context.
}

D. browser.html Worker wrapper and MessageChannel handshake

Install this before appending engine/embedder.js, and only in pthread GPU bitmap mode.

JavaScript
const bibPresent = {
  mode: presentMode,
  worker: null,
  port: null,
  bound: false,
  frames: 0,
  lastFrameId: 0
};

function installBibPresentWorkerHook() {
  if (presentMode !== "gpu-bitmap")
    return;

  const NativeWorker = window.Worker;

  window.Worker = class BIBPresentWorker extends NativeWorker {
    constructor(url, options) {
      super(url, options);

      this.addEventListener("message", (e) => {
        const d = e.data || {};

        // Sent by engine-pre.js from the actual proxied-main pthread worker.
        // Emscripten may create other Workers; ignore them unless they send
        // this exact custom hello.
        if (d.__bibPresent !== "hello")
          return;

        if (bibPresent.bound)
          return;

        bibPresent.bound = true;
        bibPresent.worker = this;

        const ch = new MessageChannel();
        bibPresent.port = ch.port1;

        ch.port1.onmessage = (ev) => {
          const m = ev.data || {};

          if (m.t !== "frame")
            return;

          const bitmap = m.bitmap;
          const id = m.id || 0;

          if (!bitmap) {
            ch.port1.postMessage({ t: "ready", id });
            return;
          }

          try {
            // Consumes ImageBitmap ownership into #screen.
            bitmapCtx.transferFromImageBitmap(bitmap);
            bibPresent.frames++;
            bibPresent.lastFrameId = id;

            // Preserve existing UI counter semantics.
            if (typeof bib !== "undefined")
              bib.frames = bibPresent.frames;
            if (typeof blitsEl !== "undefined")
              blitsEl.textContent = bibPresent.frames;
          } catch (err) {
            console.error("BIB: bitmap present failed:", err);
            try { bitmap.close(); } catch (_) {}
          } finally {
            // One-in-flight release. Worker may now transfer the next frame.
            ch.port1.postMessage({ t: "ready", id });
          }
        };

        if (ch.port1.start)
          ch.port1.start();

        // Transfer port2 into the engine pthread worker. This message travels
        // over Emscripten's Worker control pipe, but it is only a one-time
        // custom sideband install. Keep the shape custom; no "cmd" field.
        this.postMessage({
          __bibPresentPort: true,
          port: ch.port2
        }, [ch.port2]);

        // Redundant initial ready; worker also self-initializes ready=true.
        ch.port1.postMessage({ t: "ready", id: 0 });
      });
    }
  };

  // Preserve static/prototype shape well enough for Emscripten-generated code.
  Object.setPrototypeOf(window.Worker, NativeWorker);
  window.Worker.prototype = NativeWorker.prototype;
}

installBibPresentWorkerHook();

E. browser.html Module fields

Keep Module.bibGPU as the authoritative C++ boot flag, but change it to use the mode-selected gpuMode. Keep bibBlit for raster only. In gpu-bitmap, bibBlit should be ignored if accidentally called.

JavaScript
window.Module = {
  bibInteractive: true,
  bibGPU: gpuMode,
  bibCanvasGPU: gpuMode && params.get("canvasgpu") !== "0",

  bibGpuFallback: () => {
    if (!gpuMode) return;
    log("engine GPU setup failed — reloading in raster mode");
    const u = new URL(location.href);
    u.searchParams.set("gpu", "0");
    location.replace(u.href);
  },

  // existing fields...
  bibWakeUp: schedulePump,
  bibArmTimer: armPumpTimer,

  // Raster receiver only. In pthread GPU bitmap mode the frame path is
  // MessagePort/ImageBitmap/bitmaprenderer, not bibBlit.
  bibBlit,

  bibReadbackReady,
  bibPersist,
  bibSeedState: null,

  // etc...
};

Update bibBlit itself like this:

JavaScript
function bibBlit(bytes, x, y, w, h) {
  if (bib.dead)
    return;

  if (presentMode !== "raster-2d") {
    // This means C++ accidentally fell back to the old readback path while
    // the host canvas is bitmaprenderer/webgl2. Do not try putImageData.
    console.warn("BIB: unexpected bibBlit in presentMode=" + presentMode);
    return;
  }

  if (!ctx)
    return;

  const pixels = new Uint8ClampedArray(bytes.buffer, bytes.byteOffset, w * h * 4);
  ctx.putImageData(new ImageData(pixels, w, h), x, y);

  if (bib.frames === 0 && demo === "hello")
    judgeHelloFrame(pixels, w, h, 0);

  bib.frames++;
  blitsEl.textContent = bib.frames;
}

C++ per-frame present call replacing FlushAndSubmit + readPixels

The current pthread GPU path in bib_render() does this inside each dirty rect:

C++
if (g_gpu)
    skgpu::ganesh::FlushAndSubmit(g_engine->surface.get());

auto dstInfo = SkImageInfo::Make(r.width(), r.height(), kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
uint8_t* dst = g_blitPixels + (static_cast<size_t>(r.y()) * kWidth + r.x()) * 4;
readBack = g_engine->surface->readPixels(SkPixmap(dstInfo, dst, kWidth * 4), r.x(), r.y());

That is the part to remove from the steady-state GPU path. The attached code confirms Approach R currently reads the Ganesh surface back into g_blitPixels and then bibPushFrameIfDirty copies those bytes to main via bibBlit. 

main

Replace it with this sequence:

if not bitmap-ready:
    return without clearing damage

layout
snapshot dirty rects
clear damage
paint dirty rects into persistent texture SkSurface
draw persistent texture SkSurface into canvas FBO0 wrapper
FlushAndSubmit(g_fbo0Surface)
transferToImageBitmap() from the worker OffscreenCanvas
postMessage(bitmap, [bitmap]) over the dedicated MessagePort

Implementation sketch.

A. Add a GPU-paint helper that does layout + damage + paint, but no readPixels

This is deliberately a split version of bib_render(). It preserves the important rule already in your code: layout happens before damage snapshot, and clearDamage() happens before paint so paint-time invalidations belong to the next frame. 

main

C++
static bool bibPaintGPUIfDirty(bool force)
{
    ASSERT(g_gpu);

    if (!bibOnEngineThread())
        return false;
    if (!g_engine || !g_engine->surface)
        return false;
    if (g_gpuLost.load(std::memory_order_acquire))
        return false;

    if (!force && !BIB::g_frameDirty && BIB::g_uploadRect.isEmpty())
        return false;

    RefPtr view = mainFrameView();
    if (!view)
        return false;

    const double layoutStart = g_perfLog ? bibNowMs() : 0;

    // Layout can itself generate damage; must run before snapshot.
    g_engine->mainFrame->protectedDocument()->updateLayoutIgnorePendingStylesheets();

    const double paintStart = g_perfLog ? bibNowMs() : 0;
    if (g_perfLog)
        g_perf.layout += paintStart - layoutStart;

    const WebCore::IntRect frameRect(0, 0, kWidth, kHeight);

    WebCore::IntRect dirty[BIB::kMaxDamageRects];
    size_t dirtyCount = 0;

    if (force) {
        dirty[dirtyCount++] = frameRect;
    } else {
        for (size_t i = 0; i < BIB::g_damageCount; ++i) {
            WebCore::IntRect r = WebCore::intersection(BIB::g_damageRects[i], frameRect);
            if (!r.isEmpty())
                dirty[dirtyCount++] = r;
        }
    }

    // In GPU mode, do not use bibScrollBlit/g_uploadRect as a CPU mirror.
    // Your current code already disables g_scrollBlit when g_gpu survives.
    WebCore::IntRect upload = force
        ? WebCore::IntRect()
        : WebCore::intersection(BIB::g_uploadRect, frameRect);

    clearDamage();

    if (!dirtyCount && upload.isEmpty())
        return false;

    WebCore::IntRect paintedBounds;
    for (size_t i = 0; i < dirtyCount; ++i) {
        const WebCore::IntRect& r = dirty[i];

        if (!paintFrameRect(r)) {
            for (size_t j = i; j < dirtyCount; ++j)
                BIB::addDamage(dirty[j]);
            BIB::g_uploadRect.unite(upload);
            BIB::g_uploadRect.unite(paintedBounds);
            return false;
        }

        paintedBounds.unite(r);
    }

    // If only upload was set, we still need a frame transfer only if the
    // visible FBO0 differs. In GPU mode g_scrollBlit is null, so upload-only
    // should normally not occur. Treat upload as full repaint safety.
    if (!upload.isEmpty() && paintedBounds.isEmpty()) {
        BIB::addDamage(frameRect);
        return false;
    }

    if (g_perfLog) {
        g_perf.paint += bibNowMs() - paintStart;
        g_perf.painted++;
    }

    return true;
}

B. Add the GPU-to-FBO0 present helper

This is what makes transferToImageBitmap capture the frame. The OffscreenCanvas ImageBitmap contains the canvas/default framebuffer image, not arbitrary Skia textures. Therefore the persistent texture SkSurface must be drawn into g_fbo0Surface first.

C++
static bool presentGPUToCanvasFBO()
{
    if (!g_gpu || !g_engine || !g_engine->surface || !g_fbo0Surface)
        return false;

    auto* glContext = WebCore::PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return false;

    const double presentStart = g_perfLog ? bibNowMs() : 0;

    SkCanvas* dst = g_fbo0Surface->getCanvas();

    // The source surface is persistent and dirty-rect-painted. FBO0 is not
    // persistent; draw the whole current backing store into it every present.
    dst->clear(SK_ColorWHITE);
    g_engine->surface->draw(dst, 0, 0);

    // Required. paintFrameRect() and the surface draw only enqueue Ganesh work.
    skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get());

    if (g_perfLog)
        g_perf.present += bibNowMs() - presentStart;

    return true;
}

C. Add C++ wrappers around worker JS readiness and transfer

C++
static bool bibBitmapPresentReady()
{
#ifdef __EMSCRIPTEN_PTHREADS__
    return EM_ASM_INT({
        return Module.bibBitmapPresentReady && Module.bibBitmapPresentReady() ? 1 : 0;
    });
#else
    return false;
#endif
}

static bool bibTransferCurrentFrameBitmap()
{
#ifdef __EMSCRIPTEN_PTHREADS__
    int r = EM_ASM_INT({
        return Module.bibTransferCurrentFrame ? Module.bibTransferCurrentFrame() : 0;
    });
    return r == 1;
#else
    return false;
#endif
}

D. Replace bibPushFrameIfDirty() with a GPU branch plus the existing raster branch

C++
static void bibPushFrameIfDirty()
{
    static bool firstFramePushed = false;

    if (g_gpu) {
#ifdef __EMSCRIPTEN_PTHREADS__
        // Backpressure check before bibPaintGPUIfDirty(), because that helper
        // snapshots and clears damage. If main is not ready, leave damage
        // armed and coalesce.
        if (!bibBitmapPresentReady())
            return;
#endif

        bool force = !firstFramePushed;

        if (!bibPaintGPUIfDirty(force))
            return;

        if (!presentGPUToCanvasFBO()) {
            BIB::addDamage(WebCore::IntRect(0, 0, kWidth, kHeight));
            return;
        }

#ifdef __EMSCRIPTEN_PTHREADS__
        if (!bibTransferCurrentFrameBitmap()) {
            BIB::addDamage(WebCore::IntRect(0, 0, kWidth, kHeight));
            return;
        }
#endif

        firstFramePushed = true;
        return;
    }

    // Existing raster path, unchanged.
    const uint8_t* pixels = bib_render(firstFramePushed ? 0 : 1);
    if (!pixels)
        return;

    firstFramePushed = true;

    const int x = g_dirtyBox[0], y = g_dirtyBox[1], w = g_dirtyBox[2], h = g_dirtyBox[3];
    if (w <= 0 || h <= 0)
        return;

    uint8_t* copy = static_cast<uint8_t*>(malloc(static_cast<size_t>(w) * h * 4));
    if (!copy)
        return;

    for (int row = 0; row < h; ++row) {
        memcpy(copy + static_cast<size_t>(row) * w * 4,
            pixels + (static_cast<size_t>(y + row) * kWidth + x) * 4,
            static_cast<size_t>(w) * 4);
    }

    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof growMemViews === "function")
            growMemViews();
        var bytes = HEAPU8.slice($0, $0 + $3 * $4 * 4);
        _bib_wasm_free($0);
        if (Module.bibBlit)
            Module.bibBlit(bytes, $1, $2, $3, $4);
    }, copy, x, y, w, h);
}

E. Keep readback only for probes/gates

Do not delete bib_request_readback() or bib_render_readback(). The probe path should remain intentionally CPU-readback-based. But it must not be on the steady-state present path.

I would change bib_render_readback() so it does not call the old GPU bib_render() path after you split GPU paint. For GPU, make it:

C++
EMSCRIPTEN_KEEPALIVE const uint8_t* bib_render_readback()
{
    if (!bibOnEngineThread())
        return nullptr;

    if (!g_gpu)
        return bib_render(1);

    if (!g_engine || !g_engine->surface)
        return nullptr;

    // Force a full repaint into the persistent texture surface.
    if (!bibPaintGPUIfDirty(true))
        return nullptr;

    auto* glContext = WebCore::PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return nullptr;

    skgpu::ganesh::FlushAndSubmit(g_engine->surface.get());

    auto dstInfo = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!g_engine->surface->readPixels(SkPixmap(dstInfo, g_blitPixels, kWidth * 4), 0, 0))
        return nullptr;

    return g_blitPixels;
}

FBO0 vs texture-backed SkSurfaces::RenderTarget

Confirm: keep the persistent browser backing store as a texture-backed SkSurfaces::RenderTarget, and draw it into FBO0 only at present time.

Do not make WebCore paint directly into FBO0 as the long-lived backing store unless you also repaint the full viewport every frame or force preserveDrawingBuffer. Your existing code is already structured correctly here: it creates a texture-backed g_engine->surface, then wraps FBO0 separately in g_fbo0Surface. The comments explicitly say the texture-backed store is not framebuffer 0 because FBO0 is undefined after composite with preserveDrawingBuffer=false, while dirty-rect painting needs stable old pixels. 

main

The exact rule is:

WebCore/Ganesh paint target:
    g_engine->surface
    SkSurfaces::RenderTarget(...)
    persistent texture-backed backing store

Display/transfer target:
    g_fbo0Surface
    SkSurfaces::WrapBackendRenderTarget(... FBOID 0 ...)
    rewritten every present from g_engine->surface

ImageBitmap source:
    worker-private OffscreenCanvas current bitmap/default framebuffer
    therefore it captures g_fbo0Surface content after presentGPUToCanvasFBO()

So the answer is slightly nuanced:

The engine should not “render WebCore directly into FBO0” as its persistent backing store. It should render the final presented frame into FBO0 every present by drawing the persistent texture surface into g_fbo0Surface. transferToImageBitmap() then captures FBO0/canvas content.

A. Keep this surface creation pattern

C++
auto info = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
auto* grContext = WebCore::PlatformDisplay::sharedDisplay().skiaGrContext();

surface = SkSurfaces::RenderTarget(
    grContext,
    skgpu::Budgeted::kNo,
    info,
    0,
    kTopLeft_GrSurfaceOrigin,
    nullptr
);

if (surface) {
    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = 0;
    fbInfo.fFormat = 0x8058; // GL_RGBA8

    auto target = GrBackendRenderTargets::MakeGL(
        kWidth,
        kHeight,
        1,
        8,
        fbInfo
    );

    g_fbo0Surface = SkSurfaces::WrapBackendRenderTarget(
        grContext,
        target,
        kBottomLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        nullptr,
        nullptr
    );
}

B. Remove explicitSwapControl/renderViaOffscreenBackBuffer from the normal bitmap path

In PlatformDisplayEmscripten.cpp, current pthread builds force:

C++
attributes.explicitSwapControl = true;
attributes.renderViaOffscreenBackBuffer = true;

The attached file says those flags are load-bearing for Approach R readback because removing them made readPixels stale. That is exactly why they should not be part of the new zero-copy bitmap path: they are preserving/interposing a buffer for readback, not helping bitmap capture. 

PlatformDisplayEmscripten

Change to:

C++
EmscriptenWebGLContextAttributes attributes;
emscripten_webgl_init_context_attributes(&attributes);
attributes.majorVersion = 2;
attributes.minorVersion = 0;
attributes.alpha = false;
attributes.depth = false;
attributes.stencil = true;
attributes.antialias = false;
attributes.preserveDrawingBuffer = false;
attributes.enableExtensionsByDefault = true;

#if defined(__EMSCRIPTEN_PTHREADS__) && defined(BIB_GPU_READBACK_PRESENT)
attributes.explicitSwapControl = true;
attributes.renderViaOffscreenBackBuffer = true;
#endif

C. Remove -sOFFSCREEN_FRAMEBUFFER=1 from the normal bitmap build

Keep -sOFFSCREENCANVAS_SUPPORT=1.

Remove -sOFFSCREEN_FRAMEBUFFER=1 from the normal pthread+bitmap build unless you keep a legacy BIB_GPU_READBACK_PRESENT build option. OFFSCREEN_FRAMEBUFFER is associated with Emscripten’s offscreen backbuffer / explicit swap machinery. The new path wants the WebGL context’s actual OffscreenCanvas drawing buffer to be what transferToImageBitmap() captures.

Practical matrix:

BIB_PTHREAD=ON + gpu bitmap:
    keep  -sOFFSCREENCANVAS_SUPPORT=1
    remove -sOFFSCREEN_FRAMEBUFFER=1
    remove explicitSwapControl
    remove renderViaOffscreenBackBuffer

BIB_PTHREAD=ON + legacy readback debug:
    keep -sOFFSCREEN_FRAMEBUFFER=1
    keep explicitSwapControl
    keep renderViaOffscreenBackBuffer

BIB_PTHREAD=OFF + gpu implicit:
    no explicitSwapControl
    no renderViaOffscreenBackBuffer

Backpressure

Use one ImageBitmap in flight. Do not queue frames.

Reason: transferToImageBitmap() creates an ImageBitmap resource. If the worker posts faster than main consumes, you accumulate GPU-backed ImageBitmaps and latency grows. Also, transferToImageBitmap() replaces the OffscreenCanvas image each time, so blindly producing multiple frames can both waste memory and obscure which frame main is actually displaying.

The policy should be:

main owns readiness
worker may produce only when ready == true
worker sets ready=false immediately before posting ImageBitmap
main calls bitmaprenderer.transferFromImageBitmap(bitmap)
main posts {t:"ready"} back to worker
if worker is not ready:
    do not paint
    do not clear damage
    return from bibPushFrameIfDirty()
    let WebCore damage coalesce

This is coalescing, not queueing. It naturally drops intermediate display opportunities while keeping the newest logical damage.

Worker-side readiness is already in the engine-pre.js block above:

JavaScript
Module.bibBitmapPresentReady = function () {
  var p = Module.__bibPresent;
  return !!(p && p.port && p.ready);
};

C++ must check readiness before damage snapshot:

C++
if (g_gpu) {
#ifdef __EMSCRIPTEN_PTHREADS__
    if (!bibBitmapPresentReady())
        return; // damage remains armed
#endif

    if (!bibPaintGPUIfDirty(!firstFramePushed))
        return;

    if (!presentGPUToCanvasFBO()) {
        BIB::addDamage(WebCore::IntRect(0, 0, kWidth, kHeight));
        return;
    }

#ifdef __EMSCRIPTEN_PTHREADS__
    if (!bibTransferCurrentFrameBitmap()) {
        BIB::addDamage(WebCore::IntRect(0, 0, kWidth, kHeight));
        return;
    }
#endif

    firstFramePushed = true;
    return;
}

Main-side ack is in the MessagePort handler:

JavaScript
try {
  bitmapCtx.transferFromImageBitmap(bitmap);
} finally {
  ch.port1.postMessage({ t: "ready", id });
}

Do not use an unbounded array of ImageBitmaps. Do not use “latest bitmap wins” by closing old bitmaps on main unless you have measured main-side jank. The simplest and best first implementation is strictly one-in-flight.

BIB_PTHREAD=OFF main-thread implicit-present

Goal: restore the old fast path where the engine owns #screen’s WebGL2 context on the browser main thread, Ganesh renders, and the browser implicitly presents when control returns to the event loop. PlatformDisplayEmscripten.cpp already has GLContext::swapBuffers() as a no-op with the comment “WebGL presents implicitly when control returns to the event loop.” 

PlatformDisplayEmscripten

A. browser.html for BIB_PTHREAD=OFF + gpu

Do not call:

JavaScript
canvas.getContext("2d")
canvas.getContext("bitmaprenderer")

In gpu-implicit mode, #screen must be untouched by host JS. The engine calls emscripten_webgl_create_context("#screen") through initializePlatformDisplayEmscripten("#screen").

The context-selection code in section 1 already handles that:

JavaScript
if (presentMode === "gpu-implicit") {
  // Do not acquire a host context.
}

B. main.cpp GPU boot target

Change GPU boot target selection to:

C++
const char* gpuCanvasSelector = nullptr;

#ifdef __EMSCRIPTEN_PTHREADS__
EM_ASM({
    try {
        if (!GL.offscreenCanvases)
            GL.offscreenCanvases = {};

        var oc = new OffscreenCanvas($0, $1);
        var info = {};
        info.offscreenCanvas = oc;
        info.canvas = oc;
        info.id = "bibgpu";
        GL.offscreenCanvases["bibgpu"] = info;
    } catch (e) {
        console.error("EMBEDDER: bibgpu OffscreenCanvas create failed: " + e);
    }
}, kWidth, kHeight);

gpuCanvasSelector = "#bibgpu";
#else
gpuCanvasSelector = "#screen";
#endif

if (WebCore::initializePlatformDisplayEmscripten(gpuCanvasSelector)) {
    installGpuContextLossHandlers();

    auto& display = WebCore::PlatformDisplay::sharedDisplay();
    g_gpu = display.skiaGLContext() && display.skiaGrContext();
}

The current attached code always creates/registers worker-private #bibgpu and calls initializePlatformDisplayEmscripten("#bibgpu") in GPU mode. That is correct only for BIB_PTHREAD=ON. For BIB_PTHREAD=OFF, target "#screen". 

main

C. context loss handler must handle both canvas kinds

Current installGpuContextLossHandlers() only looks up GL.offscreenCanvases["bibgpu"]. Make it conditional.

C++
static void installGpuContextLossHandlers()
{
#ifdef __EMSCRIPTEN_PTHREADS__
    EM_ASM({
        var entry = typeof GL !== "undefined" &&
            GL.offscreenCanvases &&
            GL.offscreenCanvases["bibgpu"];

        var canvas = entry ? (entry.offscreenCanvas || entry.canvas) : null;
        if (!canvas) {
            out("EMBEDDER: gpu loss handlers NOT installed (no #bibgpu canvas)");
            return;
        }

        var deadline = null;
        canvas.addEventListener("webglcontextlost", function(e) {
            e.preventDefault();
            _bib_gpu_context_lost();
            deadline = setTimeout(function() {
                deadline = null;
                _bib_gpu_restore_timed_out();
            }, 8000);
        });

        canvas.addEventListener("webglcontextrestored", function() {
            if (deadline) {
                clearTimeout(deadline);
                deadline = null;
            }
            _bib_gpu_context_restored();
        });
    });
#else
    EM_ASM({
        var canvas = document.getElementById("screen");
        if (!canvas) {
            out("EMBEDDER: gpu loss handlers NOT installed (no #screen)");
            return;
        }

        var deadline = null;
        canvas.addEventListener("webglcontextlost", function(e) {
            e.preventDefault();
            _bib_gpu_context_lost();
            deadline = setTimeout(function() {
                deadline = null;
                _bib_gpu_restore_timed_out();
            }, 8000);
        });

        canvas.addEventListener("webglcontextrestored", function() {
            if (deadline) {
                clearTimeout(deadline);
                deadline = null;
            }
            _bib_gpu_context_restored();
        });
    });
#endif
}

D. surface creation for BIB_PTHREAD=OFF

Use the same two-surface structure:

C++
g_engine->surface:
    texture-backed SkSurfaces::RenderTarget
    stable persistent backing store for dirty-rect painting

g_fbo0Surface:
    WrapBackendRenderTarget(FBO 0)
    present target

Then the non-pthread GPU branch of bibPushFrameIfDirty() does:

C++
if (g_gpu) {
    bool force = !firstFramePushed;

    if (!bibPaintGPUIfDirty(force))
        return;

    if (!presentGPUToCanvasFBO()) {
        BIB::addDamage(WebCore::IntRect(0, 0, kWidth, kHeight));
        return;
    }

#ifndef __EMSCRIPTEN_PTHREADS__
    // No bitmap. Browser will present #screen WebGL implicitly when this
    // call stack returns to the browser event loop.
#endif

    firstFramePushed = true;
    return;
}

This is not literally “paint WebCore directly into default framebuffer.” It is the cleaner version: dirty-rect paint into the stable texture SkSurface, then one GPU-GPU full-frame draw into FBO0. It should be very close to the original fast path and much safer than using FBO0 as the persistent backing store.

If you want to spike the absolute old direct-FBO path for MotionMark parity, make it a separate experimental mode:

BIB_GPU_DIRECT_FBO0=1:
    g_engine->surface = g_fbo0Surface
    force full-frame paint every presented frame
    no dirty-rect persistence assumptions

But I would not ship that first.

Final recommendation

Ship pthread + bitmap as the primary architecture. Keep BIB_PTHREAD=OFF + GPU implicit as a performance/reference/no-SAB build, not as the default for real modern pages.

Reasoning:

The main-thread GPU path can recover your old 109-MotionMark-class numbers for graphics-heavy benchmarks because there is no worker-to-main transfer and no ImageBitmap hop. But this project’s harder constraint is not MotionMark; it is running a JIT-less WebKit/JSC CLoop browser inside a tab without freezing the host browser UI. Your own code moved all engine work to a dedicated pthread and added self-proxying exports specifically to prevent host-thread races and keep the page responsive. 

main

Recommended default matrix:

Default human interactive build:
    BIB_PTHREAD=ON
    gpu=1 when OffscreenCanvas + WebGL2 + bitmaprenderer exist
    present = ImageBitmap/bitmaprenderer
    fallback = raster-putImageData

No-SAB / compatibility / benchmark build:
    BIB_PTHREAD=OFF
    gpu=1
    present = implicit #screen WebGL
    warning: guest long tasks can freeze host UI

Automation / pixel-exact gates:
    gpu=0 unless explicitly testing GPU
    present = raster-putImageData

Top 3 risks and mitigations:

Risk 1: transferToImageBitmap captures stale or blank frames.

Cause: you painted into g_engine->surface but did not draw that texture-backed surface into the OffscreenCanvas default framebuffer/FBO0 before transfer.

Mitigation: make present order mandatory:

paintFrameRect dirty rects into g_engine->surface
g_engine->surface->draw(g_fbo0Surface->getCanvas(), 0, 0)
skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get())
Module.bibTransferCurrentFrame()

Do not call transferToImageBitmap() directly after painting only into the texture surface.

Risk 2: Emscripten pthread protocol collision.

Cause: using the raw pthread Worker postMessage stream for recurring frame messages or relying on Module.PThread.runningWorkers.

Mitigation: raw Worker stream only gets:

worker -> main: { __bibPresent: "hello" }
main -> worker: { __bibPresentPort: true, port }

Then all recurring frame traffic goes over MessagePort. Custom messages must not contain a cmd field.

Risk 3: main cannot consume frames as fast as worker paints.

Cause: unbounded ImageBitmap production causes GPU memory growth and display latency.

Mitigation: strict one-in-flight protocol. Worker checks ready before painting and before clearDamage(). Main posts ready only after bitmaprenderer.transferFromImageBitmap() returns. When not ready, worker returns without painting; damage stays armed and coalesces.

Final implementation note:

The important conceptual fix is this:

Approach R:
    Ganesh texture surface -> FlushAndSubmit -> readPixels -> putImageData
    bad: CPU readback, sync, flaky

New pthread GPU:
    Ganesh texture surface -> draw to OffscreenCanvas FBO0 -> FlushAndSubmit
    -> transferToImageBitmap -> MessagePort transfer -> bitmaprenderer
    good: zero CPU readback, one frame in flight

New mainthread GPU:
    Ganesh texture surface -> draw to #screen FBO0 -> FlushAndSubmit
    -> return to browser event loop
    good: fastest, but host UI can be blocked by CLoop guest JS

