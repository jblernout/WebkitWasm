/*
 * G1 GPU context spike (decision-005) — prove the Skia Ganesh GL stack over
 * a WebGL2 context OUTSIDE WebCore, against the exact libSkia.a the engine
 * links. Throwaway allowed; findings feed G2.
 *
 * What this must prove:
 *   1. emscripten_webgl_create_context → GLES3-over-WebGL2 context.
 *   2. GrGLMakeAssembledGLESInterface + emscripten_webgl_get_proc_address.
 *      (NOT the WebGL interface: the archive is compiled with
 *      SK_ASSUME_GL_ES=1, which hardwires GR_IS_GR_WEBGL()==false and stubs
 *      GrGLMakeAssembledWebGLInterface to nullptr — GrGLTypes.h:30.)
 *   3. GrDirectContexts::MakeGL — caps probing survives Emscripten's GL.
 *   4. SkSurfaces::WrapBackendRenderTarget on framebuffer 0.
 *   5. Real draws: gradient, AA paths, raster→texture image upload (the
 *      same GL-side primitive glyph-atlas text uses — SkFont itself is
 *      skipped: fontmgr+fontconfig is engine plumbing, not GL surface).
 *   6. Skia-side GPU readback (surface->readPixels) — the G3 probe path.
 *   7. OQ2: is glFenceSync resolvable (GLFence viability for G2)?
 *
 * Verdict line greppable by the probe: "GPUSPIKE: PASS ..." / "GPUSPIKE: FAIL ...".
 */

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <cstdio>
#include <cstring>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradientShader.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

// Spike-only diagnostics: private headers to pinpoint WHERE backend-RT
// wrapping fails (caps table vs proxy vs device). Not for G2 production use.
#include "src/gpu/ganesh/GrCaps.h"
#include "src/gpu/ganesh/GrDirectContextPriv.h"
#include "src/gpu/ganesh/gl/GrGLUtil.h"

static constexpr int kW = 800;
static constexpr int kH = 600;

// Outlive main(): the rAF redraw loop (emscripten_set_main_loop) keeps
// drawing after main returns, which is also how G2 will behave.
static sk_sp<GrDirectContext> g_grContext;
static sk_sp<SkSurface> g_surface;
static sk_sp<SkImage> g_uploadImage;
static int g_frame = 0;

// Emscripten reports GL_VERSION as "OpenGL ES 3.0 (WebGL 2.0 (...))".
// Skia's parser (GrGLUtil.cpp:95) matches the "(WebGL %d.%d" tail and takes
// the WEBGL number — the context gets capped at ES 2.0 and GrGLCaps drops
// RGBA8 renderability (GrGLCaps.cpp:1524). That branch exists for Skia's
// WebGL-standard builds, which SK_ASSUME_GL_ES=1 compiles out of our
// archive. Serve the string without the parenthetical so the plain GLES
// branch parses the real ES level. GL_SHADING_LANGUAGE_VERSION is safe:
// its parser matches "OpenGL ES GLSL ES %d.%d" before any WebGL tail.
static const GLubyte* spikeGetString(GLenum name)
{
    const GLubyte* s = glGetString(name);
    if (name == GL_VERSION && s) {
        static char fixed[64];
        if (!fixed[0]) {
            const char* src = reinterpret_cast<const char*>(s);
            size_t i = 0;
            for (; src[i] && src[i] != '(' && i < sizeof(fixed) - 1; ++i)
                fixed[i] = src[i];
            fixed[i] = '\0';
        }
        return reinterpret_cast<const GLubyte*>(fixed);
    }
    return s;
}

// Emscripten's glGetInternalformativ is a no-op stub: it resolves via
// getProc but never writes params, so GrGLCaps's per-format MSAA
// sample-count query (GrGLCaps.cpp initFormatTable tail) reads 0 and every
// format ends up with an EMPTY fColorSampleCounts → isFormatRenderable()
// is false at ANY sample count, killing WrapBackendRenderTarget. WebGL2
// has the real query (getInternalformatParameter, SAMPLES only — WebGL2
// dropped NUM_SAMPLE_COUNTS; the array length serves as the count).
// 0x8D41 = GL_RENDERBUFFER, 0x80A9 = GL_SAMPLES, 0x9380 = GL_NUM_SAMPLE_COUNTS.
EM_JS(void, bibQueryInternalformat, (uint32_t ifmt, uint32_t pname, int bufSize, uintptr_t outPtr), {
    var ctx = GL.currentContext.GLctx;
    var arr = ctx.getInternalformatParameter(0x8D41, ifmt, 0x80A9);
    if (!arr) {
        // GrGLCaps reads NUM_SAMPLE_COUNTS from an uninitialized stack int
        // right after this call — never leave it unwritten (Codex).
        if (bufSize >= 1)
            HEAP32[outPtr >> 2] = 0;
        return;
    }
    if (pname == 0x9380) {
        if (bufSize >= 1)
            HEAP32[outPtr >> 2] = arr.length;
    } else if (pname == 0x80A9) {
        for (var i = 0; i < Math.min(bufSize, arr.length); i++)
            HEAP32[(outPtr >> 2) + i] = arr[i];
    }
});

static void spikeGetInternalformativ(GLenum target, GLenum internalformat, GLenum pname, GLsizei bufSize, GLint* params)
{
    (void)target; // GrGLCaps only ever queries GL_RENDERBUFFER
    bibQueryInternalformat(internalformat, pname, bufSize, reinterpret_cast<uintptr_t>(params));
}

static GrGLFuncPtr getProc(void*, const char name[])
{
    if (!strcmp(name, "glGetString"))
        return reinterpret_cast<GrGLFuncPtr>(spikeGetString);
    if (!strcmp(name, "glGetInternalformativ"))
        return reinterpret_cast<GrGLFuncPtr>(spikeGetInternalformativ);
    void* p = emscripten_webgl_get_proc_address(name);
    // Misses on optional-extension functions are expected and harmless;
    // the log tells us exactly which ones Emscripten's GL cannot resolve.
    if (!p)
        std::printf("GPUSPIKE: getProc MISS %s\n", name);
    return reinterpret_cast<GrGLFuncPtr>(p);
}

static void drawFrame(SkCanvas* canvas, const sk_sp<SkImage>& uploadImage, int frame)
{
    canvas->clear(SkColorSetRGB(0x10, 0x10, 0x20));

    // Linear gradient across the top half.
    {
        SkPoint pts[2] = { { 0, 0 }, { kW, 0 } };
        SkColor colors[2] = { SK_ColorRED, SK_ColorBLUE };
        SkPaint paint;
        paint.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
        canvas->drawRect(SkRect::MakeXYWH(0, 0, kW, kH / 2.f), paint);
    }

    // Solid green AA circle, animated x so steady-state frames aren't no-ops.
    {
        SkPaint paint;
        paint.setColor(SK_ColorGREEN);
        paint.setAntiAlias(true);
        canvas->drawCircle(200 + (frame % 30), 450, 80, paint);
    }

    // Raster image upload + sampled draw (glyph-atlas-class GL work).
    canvas->drawImageRect(uploadImage, SkRect::MakeXYWH(560, 380, 160, 160), SkSamplingOptions(SkFilterMode::kLinear));

    // Stroked AA path — exercises the stencil/tessellation-ish path code.
    {
        SkPath path = SkPathBuilder()
            .moveTo(420, 560)
            .cubicTo(460, 380, 540, 580, 760, 360)
            .detach();
        SkPaint paint;
        paint.setColor(SK_ColorYELLOW);
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(6);
        canvas->drawPath(path, paint);
    }
}

int main()
{
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;
    attrs.alpha = false;
    attrs.depth = false;
    attrs.stencil = true;   // Ganesh wants stencil on the wrapped FBO for path clipping
    attrs.antialias = false; // Skia does its own AA; no MSAA on FBO 0
    attrs.preserveDrawingBuffer = true; // spike-only: lets the probe readPixels post-hoc
    attrs.enableExtensionsByDefault = true;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctxHandle = emscripten_webgl_create_context("#spike", &attrs);
    if (ctxHandle <= 0) {
        std::printf("GPUSPIKE: FAIL create_context err=%d\n", static_cast<int>(ctxHandle));
        return 1;
    }
    emscripten_webgl_make_context_current(ctxHandle);
    emscripten_set_webglcontextlost_callback("#spike", nullptr, false,
        [](int, const void*, void*) -> bool {
            std::printf("GPUSPIKE: C webglcontextlost at frame=%d\n", g_frame);
            return false;
        });

    std::printf("GPUSPIKE: GL_VERSION=%s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("GPUSPIKE: GL_RENDERER=%s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // OQ2 (decision-005): GLFence needs fenceSync — resolvable under WebGL2?
    const bool hasFence = emscripten_webgl_get_proc_address("glFenceSync")
        && emscripten_webgl_get_proc_address("glClientWaitSync")
        && emscripten_webgl_get_proc_address("glDeleteSync");

    sk_sp<const GrGLInterface> iface = GrGLMakeAssembledGLESInterface(nullptr, getProc);
    if (!iface) {
        std::printf("GPUSPIKE: FAIL GrGLMakeAssembledGLESInterface returned null (see MISS lines)\n");
        return 1;
    }
    std::printf("GPUSPIKE: GLES interface assembled\n");

    sk_sp<GrDirectContext> grContext = GrDirectContexts::MakeGL(iface);
    if (!grContext) {
        std::printf("GPUSPIKE: FAIL GrDirectContexts::MakeGL returned null\n");
        return 1;
    }
    std::printf("GPUSPIKE: GrDirectContext OK\n");

    GLint stencilBits = 0, samples = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    glGetIntegerv(GL_SAMPLES, &samples);

    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = 0;
    fbInfo.fFormat = GL_RGBA8;
    GrBackendRenderTarget backendRT = GrBackendRenderTargets::MakeGL(kW, kH, samples, stencilBits, fbInfo);

    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(
        grContext.get(), backendRT, kBottomLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType, nullptr, nullptr);
    if (!surface) {
        const GrCaps* caps = grContext->priv().caps();
        GrBackendFormat fmt = backendRT.getBackendFormat();
        std::printf("GPUSPIKE: FAIL WrapBackendRenderTarget (stencil=%d samples=%d)\n", stencilBits, samples);
        std::printf("GPUSPIKE: diag rtValid=%d fmtValid=%d compatible=%d renderable=%d rtSamples=%d\n",
            backendRT.isValid(), fmt.isValid(),
            caps->areColorTypeAndFormatCompatible(GrColorType::kRGBA_8888, fmt),
            caps->isFormatAsColorTypeRenderable(GrColorType::kRGBA_8888, fmt, backendRT.sampleCnt()),
            backendRT.sampleCnt());
        GrGLDriverInfo di = GrGLGetDriverInfo(iface.get());
        GLint numSampleCounts = -1;
        glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &numSampleCounts);
        std::printf("GPUSPIKE: diag2 parsedVer=0x%x fmtRenderable=%d rtSampleCnt=%d glNumSampleCounts=%d\n",
            di.fVersion,
            caps->isFormatRenderable(fmt, 1),
            caps->getRenderTargetSampleCount(1, fmt),
            numSampleCounts);
        return 1;
    }
    std::printf("GPUSPIKE: SkSurface wrapped FBO0 (stencil=%d samples=%d)\n", stencilBits, samples);

    // Raster-backed image for the upload test: 64x64, solid magenta center.
    sk_sp<SkImage> uploadImage;
    {
        sk_sp<SkSurface> raster = SkSurfaces::Raster(SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
        raster->getCanvas()->clear(SK_ColorMAGENTA);
        SkPaint border;
        border.setColor(SK_ColorWHITE);
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(4);
        raster->getCanvas()->drawRect(SkRect::MakeXYWH(2, 2, 60, 60), border);
        uploadImage = raster->makeImageSnapshot();
    }

    // Frame 0 carries all shader compiles; steady state is the real number.
    double t0 = emscripten_get_now();
    drawFrame(surface->getCanvas(), uploadImage, 0);
    skgpu::ganesh::FlushAndSubmit(surface.get());
    glFinish();
    double frame0Ms = emscripten_get_now() - t0;

    constexpr int kFrames = 30;
    double tSteady = emscripten_get_now();
    for (int i = 1; i <= kFrames; ++i) {
        drawFrame(surface->getCanvas(), uploadImage, i);
        skgpu::ganesh::FlushAndSubmit(surface.get());
    }
    glFinish();
    double steadyMs = (emscripten_get_now() - tSteady) / kFrames;

    // Skia-side GPU readback (the G3 gate/probe path): sample known pixels.
    // Circle center animates x ∈ [200, 229] with radius 80, so (230, 450)
    // is inside it on every frame; image block at (560..720, 380..540) →
    // magenta around (640, 460).
    uint32_t px[4] = { 0, 0, 0, 0 }; // [0]=gradient-left [1]=circle [2]=image [3]=background
    SkImageInfo onePx = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    bool readOK = surface->readPixels(onePx, &px[0], 4, 40, 150)
        && surface->readPixels(onePx, &px[1], 4, 230, 450)
        && surface->readPixels(onePx, &px[2], 4, 640, 460)
        && surface->readPixels(onePx, &px[3], 4, 780, 590);
    if (!readOK) {
        std::printf("GPUSPIKE: FAIL surface->readPixels\n");
        return 1;
    }

    // RGBA byte order in memory (kRGBA_8888): R=byte0 … A=byte3.
    auto r = [](uint32_t p) { return p & 0xff; };
    auto g = [](uint32_t p) { return (p >> 8) & 0xff; };
    auto b = [](uint32_t p) { return (p >> 16) & 0xff; };
    std::printf("GPUSPIKE: px gradient=(%u,%u,%u) circle=(%u,%u,%u) image=(%u,%u,%u) bg=(%u,%u,%u)\n",
        r(px[0]), g(px[0]), b(px[0]), r(px[1]), g(px[1]), b(px[1]),
        r(px[2]), g(px[2]), b(px[2]), r(px[3]), g(px[3]), b(px[3]));

    const bool gradientOK = r(px[0]) > 180 && b(px[0]) < 80;            // near-red at x=40
    const bool circleOK = g(px[1]) > 180 && r(px[1]) < 80;              // pure green
    const bool imageOK = r(px[2]) > 180 && b(px[2]) > 180 && g(px[2]) < 80; // magenta
    const bool bgOK = r(px[3]) < 40 && g(px[3]) < 40;                   // dark background

    if (gradientOK && circleOK && imageOK && bgOK) {
        std::printf("GPUSPIKE: PASS gl=%s fence=%s stencil=%d samples=%d frame0=%.1fms steady=%.2fms\n",
            reinterpret_cast<const char*>(glGetString(GL_VERSION)),
            hasFence ? "yes" : "no", stencilBits, samples, frame0Ms, steadyMs);
    } else {
        std::printf("GPUSPIKE: FAIL pixel check gradient=%d circle=%d image=%d bg=%d\n",
            gradientOK, circleOK, imageOK, bgOK);
        return 1;
    }

    // Keep redrawing on rAF: a WebGL canvas presents per animation frame —
    // a one-shot synchronous draw never reliably reaches the compositor.
    // This is also the shape of the real engine loop in G2.
    g_grContext = grContext;
    g_surface = surface;
    g_uploadImage = uploadImage;
    emscripten_set_main_loop([]() {
        drawFrame(g_surface->getCanvas(), g_uploadImage, ++g_frame);
        skgpu::ganesh::FlushAndSubmit(g_surface.get());
        // TEMP diagnostic: confirm FBO 0 itself holds the pixels C-side.
        if (g_frame % 120 == 1) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            unsigned char p[4] = { 0, 0, 0, 0 };
            glReadPixels(40, kH - 1 - 150, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
            std::printf("GPUSPIKE: fbo0 raw readback frame=%d rgba=(%u,%u,%u,%u) glErr=0x%x\n",
                g_frame, p[0], p[1], p[2], p[3], glGetError());
            g_grContext->resetContext(kRenderTarget_GrGLBackendState);
        }
    }, 0 /* fps: use rAF */, 0 /* don't block */);
    return 0;
}
