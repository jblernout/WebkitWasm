// BrowserInBrowser Phase 2 embedder: the make-or-break gate.
// Builds a WebCore::Page against internal headers (no public embedding API
// exists outside Cocoa — decision-001), loads a fixed HTML string, lays it
// out, and paints through a Skia raster surface. Output is a PPM dump in the
// wasm FS plus a render-tree text dump on stdout, so layout correctness is
// verifiable even before fonts are packaged.
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
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/ProcessPrivilege.h>

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

// Dump RGBA pixels as a binary PPM (P6, alpha dropped) into the wasm FS.
// The node runner extracts it afterwards.
static bool writePPM(const char* path, const SkPixmap& pixmap)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", pixmap.width(), pixmap.height());
    for (int y = 0; y < pixmap.height(); ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(pixmap.addr(0, y));
        for (int x = 0; x < pixmap.width(); ++x) {
            // kRGBA_8888 byte order is R,G,B,A regardless of endianness.
            fwrite(row + x * 4, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

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
    surface->getCanvas()->clear(SK_ColorWHITE);

    {
        WebCore::GraphicsContextSkia context(*surface->getCanvas(), WebCore::RenderingMode::Unaccelerated, WebCore::RenderingPurpose::Unspecified);
        frameView->paint(context, WebCore::IntRect(0, 0, kWidth, kHeight));
    }

    printf("EMBEDDER: paint OK\n");

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) {
        printf("EMBEDDER: FAIL peekPixels\n");
        return 1;
    }
    if (!writePPM("/out.ppm", pixmap)) {
        printf("EMBEDDER: FAIL writePPM\n");
        return 1;
    }

    // Cheap signal for the runner: count pixels that are not pure white.
    int nonWhite = 0;
    for (int y = 0; y < kHeight; ++y) {
        const uint32_t* row = static_cast<const uint32_t*>(pixmap.addr(0, y));
        for (int x = 0; x < kWidth; ++x) {
            if ((row[x] & 0x00FFFFFF) != 0x00FFFFFF)
                ++nonWhite;
        }
    }
    printf("EMBEDDER: nonWhitePixels=%d\n", nonWhite);
    printf("EMBEDDER: DONE\n");
    return 0;
}
