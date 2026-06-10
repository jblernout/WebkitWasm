// BrowserInBrowser embedder.
// Builds a WebCore::Page against internal headers (no public embedding API
// exists outside Cocoa — decision-001), loads a fixed HTML string, lays it
// out, and paints through a Skia raster surface.
//
// Two modes, decided at runtime by Module.bibInteractive:
//  - gate mode (node, tools/run-embedder.cjs): one paint → PPM dump in the
//    wasm FS + region-pixel assertions → exit. The Phase 2 gate, unchanged.
//  - interactive mode (browser host page, web/browser.html): main() sets the
//    page up, signals Module.onEngineReady, then keeps the runtime alive.
//    The host drives bib_tick()/bib_render() from requestAnimationFrame and
//    blits the returned RGBA buffer to a <canvas> via putImageData.
//
// Construction sequence is cribbed from SVGImage::dataChanged() — the one
// in-tree user of pageConfigurationWithEmptyClients() that paints.

#include "config.h"

#include "CommonAtomStrings.h"
#include "Document.h"
#include "DocumentLoader.h"
#include "DocumentView.h" // inline LocalFrame::view() lives here, not in LocalFrame.h
#include "DocumentWriter.h"
#include "EmptyClients.h"
#include "FrameLoader.h"
#include "GraphicsContextSkia.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "RenderTreeAsText.h"
#include "Settings.h"
#include "SharedBuffer.h"
#include <JavaScriptCore/InitializeThreading.h>
#include <emscripten.h>
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/ProcessPrivilege.h>
#include <wtf/RunLoop.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkSurface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#include <cstdio>

namespace BIB {
void installEmbedderStrategies(); // EmbedderStrategies.cpp
}

static constexpr int kWidth = 800;
static constexpr int kHeight = 600;

static const char* kTestHTML =
    "<!DOCTYPE html>"
    "<html><head><title>gate</title></head>"
    "<body style='background:#ffffff; margin:0'>"
    "<h1 style='color:#cc0000; font-size:48px; margin:20px'>hello</h1>"
    "<div style='width:200px; height:100px; background:#0066cc'></div>"
    "</body></html>";

// Engine state outlives main() in interactive mode. Intentionally leaked:
// destruction order of WebCore globals at process exit is not a supported
// path in this embedder, and in gate mode the process exits anyway.
struct Engine {
    RefPtr<WebCore::Page> page;
    RefPtr<WebCore::LocalFrame> mainFrame;
    RefPtr<WebCore::LocalFrameView> frameView;
    sk_sp<SkSurface> surface;
};
static Engine* g_engine;

// bib_render() hands this buffer to JS. Unpremultiplied RGBA as ImageData
// expects; for this engine's output (opaque pixels) conversion is identity.
static uint8_t g_blitPixels[kWidth * kHeight * 4];

// Dump RGBA pixels as a binary PPM (P6, alpha dropped) into the wasm FS.
// The node runner extracts it afterwards.
static bool writePPM(const char* path, const SkPixmap& pixmap)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fprintf(f, "P6\n%d %d\n255\n", pixmap.width(), pixmap.height()) > 0;
    for (int y = 0; ok && y < pixmap.height(); ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(pixmap.addr(0, y));
        for (int x = 0; x < pixmap.width(); ++x) {
            // kRGBA_8888 byte order is R,G,B,A regardless of endianness.
            if (fwrite(row + x * 4, 1, 3, f) != 3) {
                ok = false;
                break;
            }
        }
    }
    ok = ok && !ferror(f);
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

// Layout (if dirty) + paint the full frame into the persistent surface.
static bool paintFrame()
{
    g_engine->mainFrame->protectedDocument()->updateLayoutIgnorePendingStylesheets();
    g_engine->surface->getCanvas()->clear(SK_ColorWHITE);
    WebCore::GraphicsContextSkia context(*g_engine->surface->getCanvas(), WebCore::RenderingMode::Unaccelerated, WebCore::RenderingPurpose::Unspecified);
    g_engine->frameView->paint(context, WebCore::IntRect(0, 0, kWidth, kHeight));
    return true;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int bib_frame_width() { return kWidth; }
EMSCRIPTEN_KEEPALIVE int bib_frame_height() { return kHeight; }

// One non-blocking engine-RunLoop iteration: fires due WebCore timers and
// dispatched main-thread functions. Drive from requestAnimationFrame.
// (RunMode::Iterate never sleeps — RunLoopGeneric's Drain-only waitUntil.)
EMSCRIPTEN_KEEPALIVE void bib_tick()
{
    WTF::RunLoop::cycle();
}

// Repaint and return the RGBA frame buffer (kWidth*kHeight*4 bytes).
// Returns 0 on failure.
EMSCRIPTEN_KEEPALIVE const uint8_t* bib_render()
{
    if (!g_engine || !paintFrame())
        return nullptr;
    auto dstInfo = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!g_engine->surface->readPixels(SkPixmap(dstInfo, g_blitPixels, kWidth * 4), 0, 0))
        return nullptr;
    return g_blitPixels;
}

} // extern "C"

int main()
{
    JSC::initialize();
    WTF::initializeMainThread();
    WTF::setProcessPrivileges(WTF::allPrivileges());
    WebCore::initializeCommonAtomStrings();
    BIB::installEmbedderStrategies();

    printf("EMBEDDER: init OK\n");

    auto pageConfiguration = WebCore::pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    auto page = WebCore::Page::create(WTF::move(pageConfiguration));
    page->settings().setScriptEnabled(false);
    page->settings().setAcceleratedCompositingEnabled(false);

    RefPtr localMainFrame = page->localMainFrame();
    if (!localMainFrame) {
        printf("EMBEDDER: FAIL no localMainFrame\n");
        return 1;
    }

    localMainFrame->setView(WebCore::LocalFrameView::create(*localMainFrame, WebCore::IntSize(kWidth, kHeight)));
    localMainFrame->init();

    RefPtr frameView = localMainFrame->view();
    frameView->setCanHaveScrollbars(false);

    Ref loader = localMainFrame->loader();
    RefPtr documentLoader = loader->activeDocumentLoader();
    if (!documentLoader) {
        printf("EMBEDDER: FAIL no activeDocumentLoader\n");
        return 1;
    }
    documentLoader->writer().setMIMEType("text/html"_s);
    documentLoader->writer().begin(URL());
    documentLoader->writer().addData(WebCore::SharedBuffer::create(unsafeMakeSpan(reinterpret_cast<const uint8_t*>(kTestHTML), strlen(kTestHTML))));
    documentLoader->writer().end();

    printf("EMBEDDER: HTML loaded\n");

    frameView->resize(kWidth, kHeight);
    localMainFrame->protectedDocument()->updateLayoutIgnorePendingStylesheets();

    printf("EMBEDDER: layout OK\n");

    // Render-tree dump: verifies parsing+style+layout even if glyph raster
    // fails (missing fonts paint nothing but still produce geometry).
    auto treeDump = WebCore::externalRepresentation(localMainFrame.get());
    printf("RENDER TREE:\n%s\n", treeDump.utf8().data());

    auto info = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) {
        printf("EMBEDDER: FAIL SkSurface\n");
        return 1;
    }

    g_engine = new Engine { WTF::move(page), WTF::move(localMainFrame), WTF::move(frameView), WTF::move(surface) };

    if (!paintFrame()) {
        printf("EMBEDDER: FAIL paint\n");
        return 1;
    }
    printf("EMBEDDER: paint OK\n");

    if (EM_ASM_INT({ return Module.bibInteractive ? 1 : 0; })) {
        // Browser mode: hand control to the host page's rAF loop. The unwind
        // skips the rest of main and keeps the runtime (and g_engine) alive.
        printf("EMBEDDER: interactive — runtime stays alive\n");
        EM_ASM({ if (Module.onEngineReady) Module.onEngineReady(); });
        emscripten_exit_with_live_runtime();
    }

    SkPixmap pixmap;
    if (!g_engine->surface->peekPixels(&pixmap)) {
        printf("EMBEDDER: FAIL peekPixels\n");
        return 1;
    }
    if (!writePPM("/out.ppm", pixmap)) {
        printf("EMBEDDER: FAIL writePPM\n");
        return 1;
    }

    // Gate assertions are region-specific so a fontconfig/glyph regression
    // cannot hide behind the div: the blue box and the red heading must BOTH
    // be present, independently (Codex review 2026-06-09).
    int nonWhite = 0;
    int exactBlue = 0; // #0066CC <div>, expected exactly 200x100
    int redGlyph = 0; // antialiased #CC0000 "hello" glyphs in the h1 band
    for (int y = 0; y < kHeight; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(pixmap.addr(0, y));
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            if (!(r == 0xFF && g == 0xFF && b == 0xFF))
                ++nonWhite;
            if (r == 0x00 && g == 0x66 && b == 0xCC)
                ++exactBlue;
            if (y < 76 && r > 0x90 && g < 0x60 && b < 0x60)
                ++redGlyph;
        }
    }
    printf("EMBEDDER: nonWhitePixels=%d exactBlue=%d redGlyph=%d\n", nonWhite, exactBlue, redGlyph);
    if (exactBlue != 200 * 100) {
        printf("EMBEDDER: FAIL blue div wrong size (%d != 20000)\n", exactBlue);
        return 1;
    }
    if (redGlyph < 200) {
        printf("EMBEDDER: FAIL heading glyphs missing (fonts broken?)\n");
        return 1;
    }
    printf("EMBEDDER: DONE\n");
    return 0;
}
