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

#include "BibIDBServer.h"
#include "BibPageClients.h"
#include "BibStorage.h"
#include "CommonAtomStrings.h"
#include "CookieJar.h"
#include "CurlContext.h"
#include "Document.h"
#include "DocumentLoader.h"
#include "DocumentView.h" // inline LocalFrame::view() lives here, not in LocalFrame.h
#include "DocumentWriter.h"
#include "EmptyClients.h"
#include "EventHandler.h"
#include "FocusController.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "GraphicsContextSkia.h"
#include "HandleUserInputEventResult.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "PlatformKeyboardEvent.h"
#include "PlatformMouseEvent.h"
#include "PlatformWheelEvent.h"
#include "RenderTreeAsText.h"
#include "ResourceRequest.h"
#include "ScriptController.h"
#include "ScrollAnimator.h"
#include "ScrollingCoordinatorTypes.h" // WheelEventProcessingSteps
#include "SecurityContext.h"
#include "Settings.h"
#include "SharedBuffer.h"
#include "StorageSessionProvider.h"
#include <JavaScriptCore/InitializeThreading.h>
#include <emscripten.h>
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ProcessPrivilege.h>
#include <wtf/RunLoop.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkSurface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#include <cstdio>
#include <cstdlib>

namespace BIB {
void installEmbedderStrategies(); // EmbedderStrategies.cpp
Ref<WebCore::StorageSessionProvider> createEmbedderStorageSessionProvider(); // EmbedderStrategies.cpp
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
    sk_sp<SkSurface> surface;
};
static Engine* g_engine;

// The main frame's CURRENT view. Never cache a LocalFrameView: every
// committed navigation replaces it (BibFrameLoaderClient::
// transitionToCommittedForNewPage), so a stored pointer would be the boot
// view forever — painting a detached view renders nothing.
static WebCore::LocalFrameView* mainFrameView()
{
    return g_engine ? g_engine->mainFrame->view() : nullptr;
}

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

// Layout + paint the full frame into the persistent surface, then clear the
// dirty flag BibChromeClient maintains.
static bool paintFrame()
{
    RefPtr view = mainFrameView();
    if (!view)
        return false;
    g_engine->mainFrame->protectedDocument()->updateLayoutIgnorePendingStylesheets();
    g_engine->surface->getCanvas()->clear(SK_ColorWHITE);
    WebCore::GraphicsContextSkia context(*g_engine->surface->getCanvas(), WebCore::RenderingMode::Unaccelerated, WebCore::RenderingPurpose::Unspecified);
    view->paint(context, WebCore::IntRect(0, 0, kWidth, kHeight));
    BIB::g_frameDirty = false;
    return true;
}

static OptionSet<WebCore::PlatformEvent::Modifier> modifiersFromBits(int bits)
{
    OptionSet<WebCore::PlatformEvent::Modifier> modifiers;
    if (bits & 1)
        modifiers.add(WebCore::PlatformEvent::Modifier::ShiftKey);
    if (bits & 2)
        modifiers.add(WebCore::PlatformEvent::Modifier::ControlKey);
    if (bits & 4)
        modifiers.add(WebCore::PlatformEvent::Modifier::AltKey);
    if (bits & 8)
        modifiers.add(WebCore::PlatformEvent::Modifier::MetaKey);
    return modifiers;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int bib_frame_width() { return kWidth; }
EMSCRIPTEN_KEEPALIVE int bib_frame_height() { return kHeight; }

// One non-blocking engine-RunLoop iteration: fires due WebCore timers and
// dispatched main-thread functions (DOM timers, RenderingUpdateScheduler's
// fallback timer, caret blink). Drive from requestAnimationFrame.
// (RunMode::Iterate never sleeps — RunLoopGeneric's Drain-only waitUntil.)
EMSCRIPTEN_KEEPALIVE void bib_tick()
{
    WTF::RunLoop::cycle();
    // Drive WebCore's "update the rendering" steps. This port has no
    // DisplayRefreshMonitor, so nothing else ever runs them — guest
    // requestAnimationFrame callbacks NEVER fired (root cause #16), which
    // silently stalled everything rAF-shaped: CSS/JS animations,
    // IntersectionObserver delivery, and rAF-deferred commits (react-helmet
    // batches <script> head insertions through rAF — 2captcha's reCAPTCHA
    // loader died exactly there). The host page calls bib_tick from its own
    // rAF loop, so this runs once per display frame; updateRendering is
    // cheap when no steps are scheduled.
    if (g_engine) {
        g_engine->page->updateRendering();
        g_engine->page->finalizeRenderingUpdate({ });
    }
}

// Returns the RGBA frame buffer (kWidth*kHeight*4) after repainting, or 0.
// force=0: only repaints (and returns the buffer) when the page is dirty —
//          the rAF blit loop skips putImageData on clean frames.
// force=1: always repaints and returns the buffer (pixel probes, first use).
EMSCRIPTEN_KEEPALIVE const uint8_t* bib_render(int force)
{
    if (!g_engine)
        return nullptr;
    if (!force && !BIB::g_frameDirty)
        return nullptr;
    if (!paintFrame())
        return nullptr;
    auto dstInfo = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!g_engine->surface->readPixels(SkPixmap(dstInfo, g_blitPixels, kWidth * 4), 0, 0))
        return nullptr;
    return g_blitPixels;
}

// --- Input forwarding (canvas events -> WebCore EventHandler) ---

EMSCRIPTEN_KEEPALIVE void bib_mouse_move(double x, double y, int modifierBits)
{
    if (!g_engine)
        return;
    WebCore::PlatformMouseEvent event({ x, y }, { x, y }, WebCore::MouseButton::None,
        WebCore::PlatformEvent::Type::MouseMoved, 0, modifiersFromBits(modifierBits),
        MonotonicTime::now(), 0, WebCore::SyntheticClickType::NoTap);
    g_engine->mainFrame->eventHandler().mouseMoved(event);
}

EMSCRIPTEN_KEEPALIVE void bib_mouse_button(int down, int jsButton, double x, double y, int clickCount, int modifierBits)
{
    if (!g_engine)
        return;
    auto button = WebCore::MouseButton::Other;
    if (jsButton == 0)
        button = WebCore::MouseButton::Left;
    else if (jsButton == 1)
        button = WebCore::MouseButton::Middle;
    else if (jsButton == 2)
        button = WebCore::MouseButton::Right;
    WebCore::PlatformMouseEvent event({ x, y }, { x, y }, button,
        down ? WebCore::PlatformEvent::Type::MousePressed : WebCore::PlatformEvent::Type::MouseReleased,
        clickCount, modifiersFromBits(modifierBits), MonotonicTime::now(), 0,
        WebCore::SyntheticClickType::NoTap);
    if (down)
        g_engine->mainFrame->eventHandler().handleMousePressEvent(event);
    else
        g_engine->mainFrame->eventHandler().handleMouseReleaseEvent(event);
}

EMSCRIPTEN_KEEPALIVE void bib_wheel(double x, double y, double deltaX, double deltaY, int modifierBits)
{
    if (!g_engine)
        return;
    auto modifiers = modifiersFromBits(modifierBits);
    // DOM wheel deltas are positive-down; PlatformWheelEvent is positive-up.
    WebCore::PlatformWheelEvent event(WebCore::IntPoint(x, y), WebCore::IntPoint(x, y),
        -deltaX, -deltaY, -deltaX / 120.0f, -deltaY / 120.0f,
        WebCore::PlatformWheelEventGranularity::ScrollByPixelWheelEvent,
        modifiers.contains(WebCore::PlatformEvent::Modifier::ShiftKey),
        modifiers.contains(WebCore::PlatformEvent::Modifier::ControlKey),
        modifiers.contains(WebCore::PlatformEvent::Modifier::AltKey),
        modifiers.contains(WebCore::PlatformEvent::Modifier::MetaKey));
    g_engine->mainFrame->eventHandler().handleWheelEvent(event,
        { WebCore::WheelEventProcessingSteps::SynchronousScrolling, WebCore::WheelEventProcessingSteps::BlockingDOMEventDispatch });
}

// Diagnostics: dump script/scroll state to stdout (host console).
EMSCRIPTEN_KEEPALIVE void bib_diag()
{
    if (!g_engine)
        return;
    auto& frame = *g_engine->mainFrame;
    RefPtr doc = frame.document();
    printf("DIAG: scriptEnabledSetting=%d\n", frame.settings().isScriptEnabled());
    printf("DIAG: canExecuteScripts=%d\n", frame.script().canExecuteScripts(WebCore::ReasonForCallingCanExecuteScripts::NotAboutToExecuteScript));
    printf("DIAG: sandboxedScripts=%d\n", doc && doc->isSandboxed(WebCore::SandboxFlag::Scripts));
    printf("DIAG: docURL=%s\n", doc ? doc->url().string().utf8().data() : "(null)");
    if (RefPtr view = mainFrameView()) {
        auto contents = view->contentsSize();
        printf("DIAG: contentsSize=%dx%d scrollY=%d\n", contents.width(), contents.height(), view->scrollPosition().y());
        printf("DIAG: isScrollableOrRubberbandable=%d canHaveScrollbars=%d vScrollbar=%d scrollAnimatorEnabled=%d\n",
            view->isScrollableOrRubberbandable(),
            view->canHaveScrollbars(),
            !!view->verticalScrollbar(),
            frame.settings().scrollAnimatorEnabled());
    }
    auto result = frame.script().executeScriptIgnoringException("6*7"_s, JSC::SourceTaintedOrigin::Untainted);
    printf("DIAG: executeScript(6*7) isNumber=%d value=%d\n", result && result.isNumber(), result && result.isNumber() ? (int)result.asNumber() : -1);
}

// Diagnostics: programmatic scroll, bypassing the wheel-event path.
EMSCRIPTEN_KEEPALIVE void bib_scroll_to(int y)
{
    if (!g_engine)
        return;
    if (RefPtr view = mainFrameView())
        view->setScrollPosition(WebCore::ScrollPosition(0, y));
}

// Diagnostic: run a script string in the guest main world. Output channel
// is the guest console (wrap probes in console.log(...) — the forwarder
// puts them on stderr as "BIB: console log: ..."). Returns 0 if the engine
// is not up. Drives DOM/computed-style probes that are otherwise
// impossible from the host side.
EMSCRIPTEN_KEEPALIVE int bib_eval(const char* source)
{
    if (!g_engine || !source)
        return 0;
    g_engine->mainFrame->script().executeScriptIgnoringException(String::fromUTF8(source), JSC::SourceTaintedOrigin::Untainted);
    return 1;
}

// Phase 4: real navigation. Drives FrameLoader::load -> DocumentLoader ->
// CachedResourceLoader -> EmbedderLoaderStrategy -> CurlRequest -> Wisp.
EMSCRIPTEN_KEEPALIVE void bib_load_url(const char* url)
{
    if (!g_engine)
        return;
    WebCore::ResourceRequest request { URL { String::fromUTF8(url) } };
    WebCore::FrameLoadRequest frameLoadRequest { *g_engine->mainFrame, WTF::move(request) };
    printf("EMBEDDER: loading %s\n", url);
    g_engine->mainFrame->loader().load(WTF::move(frameLoadRequest));
}

// type: 0 = RawKeyDown, 1 = KeyUp, 2 = Char. Strings are UTF-8 (use ccall).
// text is only meaningful for Char events.
EMSCRIPTEN_KEEPALIVE int bib_key(int type, const char* key, const char* code, const char* text, int windowsVirtualKeyCode, int isAutoRepeat, int modifierBits)
{
    if (!g_engine)
        return 0;
    auto eventType = WebCore::PlatformEvent::Type::RawKeyDown;
    if (type == 1)
        eventType = WebCore::PlatformEvent::Type::KeyUp;
    else if (type == 2)
        eventType = WebCore::PlatformEvent::Type::Char;
    String textString = eventType == WebCore::PlatformEvent::Type::Char ? String::fromUTF8(text) : String();
    WebCore::PlatformKeyboardEvent event(eventType, textString, textString,
        String::fromUTF8(key), String::fromUTF8(code), emptyString(),
        windowsVirtualKeyCode, isAutoRepeat, false, false,
        modifiersFromBits(modifierBits), MonotonicTime::now());
    return g_engine->mainFrame->eventHandler().keyEvent(event);
}

} // extern "C"

int main()
{
    // Verbose libcurl tracing (?curldebug=1 on the host page). Set from C
    // because Module.ENV-based getenv proved unreliable here. MUST happen
    // before installEmbedderStrategies(): the cookie-session setup
    // constructs the CurlContext singleton, which reads DEBUG_CURL exactly
    // once in its constructor.
    if (EM_ASM_INT({ return Module.bibCurlDebug ? 1 : 0; }))
        setenv("DEBUG_CURL", "1", 1);
    printf("EMBEDDER: curldebug=%s\n", getenv("DEBUG_CURL") ? "on" : "off");
    if (getenv("DEBUG_CURL")) {
        // Constructs the CurlContext singleton NOW (post-setenv) and reports
        // whether the verbose flag actually latched — splits env plumbing
        // from curl-output plumbing when tracing goes missing.
        printf("EMBEDDER: curl verbose=%d\n", WebCore::CurlContext::singleton().isVerbose());
    }

    JSC::initialize();
    WTF::initializeMainThread();
    WTF::setProcessPrivileges(WTF::allPrivileges());
    WebCore::initializeCommonAtomStrings();
    BIB::installEmbedderStrategies();

    const bool interactive = EM_ASM_INT({ return Module.bibInteractive ? 1 : 0; });

    printf("EMBEDDER: init OK\n");

    auto pageConfiguration = WebCore::pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    pageConfiguration.chromeClient = makeUniqueRef<BIB::BibChromeClient>();
    pageConfiguration.editorClient = makeUniqueRef<BIB::BibEditorClient>();
    // Real cookie jar over the embedder's in-memory NetworkStorageSession.
    // The empty-clients default wraps EmptyStorageSessionProvider (null
    // session): document.cookie writes vanish, reads return "" — Google
    // serves its "Cookies are disabled" interstitial on exactly that.
    pageConfiguration.cookieJar = WebCore::CookieJar::create(BIB::createEmbedderStorageSessionProvider());
    // Real in-memory localStorage/sessionStorage (root cause #11): the
    // empty-clients provider discards writes, and the WebCore-layer
    // defaults leave both window properties OFF entirely (settings flipped
    // below) — `window.localStorage` was a ReferenceError that modern site
    // bootstraps treat as fatal.
    pageConfiguration.storageNamespaceProvider = BIB::BibStorageNamespaceProvider::create();
    // Real in-process IndexedDB (root cause #13): the empty-clients
    // DatabaseProvider RELEASE_ASSERTs on idbConnectionToServerForSession —
    // any guest JS touching window.indexedDB killed that page's script
    // (discord.com/login went blank exactly there). WebKitLegacy's
    // InProcessIDBServer recipe, in-memory backing store.
    pageConfiguration.databaseProvider = BIB::BibDatabaseProvider::create();

    // pageConfigurationWithEmptyClients hardcodes SandboxFlags::all() on the
    // main frame (it exists for SVGImage, which must never run script) —
    // Document::initSecurityContext copies the frame's flags, silently
    // blocking ALL script regardless of Settings::setScriptEnabled. Clear
    // them for the interactive embedder; gate mode keeps SVG semantics.
    if (interactive) {
        if (auto* localParams = std::get_if<WebCore::PageConfiguration::LocalMainFrameCreationParameters>(&pageConfiguration.mainFrameCreationParameters)) {
            localParams->effectiveSandboxFlags = { };
            // Phase 4: the empty frame client silently kills real loads four
            // ways (dropped policy completions, canHandleRequest=false,
            // canShowMIMEType=false, committedLoad no-op) — install ours.
            localParams->clientCreator = [](auto&, auto& frameLoader) -> UniqueRef<WebCore::LocalFrameLoaderClient> {
                return WTF::makeUniqueRefWithoutRefCountedCheck<BIB::BibFrameLoaderClient>(frameLoader);
            };
        }
    }
    auto page = WebCore::Page::create(WTF::move(pageConfiguration));
    // Script stays OFF in gate mode so the offscreen pixel gate is
    // byte-stable; interactive mode runs JSC (CLoop) inside the page.
    page->settings().setScriptEnabled(interactive);
    page->settings().setAcceleratedCompositingEnabled(false);
    // Raw WebCore defaults this to FALSE (embedders must opt in) —
    // without it CachedResourceLoader silently DEFERS every non-data:
    // image load: no request, no error event, blank <img> (root cause #8,
    // empty-defaults family).
    page->settings().setLoadsImagesAutomatically(true);
    // Raw-WebCore defaults again: LocalStorageEnabled/SessionStorageEnabled
    // are FALSE at this layer (the WebKit wrappers flip them on; we must
    // too). The window properties are IDL-gated behind these settings.
    page->settings().setLocalStorageEnabled(true);
    page->settings().setSessionStorageEnabled(true);
    // requestIdleCallback defaults FALSE at every preferences layer (same
    // family as root cause #11) although WebCore fully implements it.
    // Lazy-hydration/scheduler libraries feature-detect it; a missing
    // global silently strands their deferred work.
    page->settings().setRequestIdleCallbackEnabled(true);

    RefPtr localMainFrame = page->localMainFrame();
    if (!localMainFrame) {
        printf("EMBEDDER: FAIL no localMainFrame\n");
        return 1;
    }

    localMainFrame->setView(WebCore::LocalFrameView::create(*localMainFrame, WebCore::IntSize(kWidth, kHeight)));
    localMainFrame->init();

    // Re-fetched after init(): the initial-empty-document commit already
    // ran transitionToCommittedForNewPage, which replaced the view above.
    RefPtr frameView = localMainFrame->view();
    frameView->setCanHaveScrollbars(interactive);

    if (interactive) {
        page->focusController().setActive(true);
        page->focusController().setFocused(true);
    }

    Ref loader = localMainFrame->loader();
    RefPtr documentLoader = loader->activeDocumentLoader();
    if (!documentLoader) {
        printf("EMBEDDER: FAIL no activeDocumentLoader\n");
        return 1;
    }

    // The host page may supply the document via Module.bibHTML (a JS
    // string); without it the built-in gate page loads. Copied out of the
    // JS heap via stringToUTF8 (forced into the runtime by
    // EXPORTED_RUNTIME_METHODS) into a malloc'd UTF-8 buffer.
    char* hostHTML = nullptr;
    int hostHTMLBytes = EM_ASM_INT({ return Module.bibHTML ? lengthBytesUTF8(Module.bibHTML) + 1 : 0; });
    if (hostHTMLBytes > 0) {
        hostHTML = static_cast<char*>(malloc(hostHTMLBytes));
        if (hostHTML)
            EM_ASM({ stringToUTF8(Module.bibHTML, $0, $1); }, hostHTML, hostHTMLBytes);
        else
            printf("EMBEDDER: bibHTML allocation failed (%d bytes) — using built-in page\n", hostHTMLBytes);
    }
    const char* html = hostHTML ? hostHTML : kTestHTML;

    documentLoader->writer().setMIMEType("text/html"_s);
    documentLoader->writer().begin(URL());
    documentLoader->writer().addData(WebCore::SharedBuffer::create(unsafeMakeSpan(reinterpret_cast<const uint8_t*>(html), strlen(html))));
    documentLoader->writer().end();
    free(hostHTML);

    printf("EMBEDDER: HTML loaded\n");

    // Re-fetch: the writer-driven load above may have replaced the view too.
    frameView = localMainFrame->view();
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

    g_engine = new Engine { WTF::move(page), WTF::move(localMainFrame), WTF::move(surface) };

    if (!paintFrame()) {
        printf("EMBEDDER: FAIL paint\n");
        return 1;
    }
    printf("EMBEDDER: paint OK\n");

    if (interactive) {
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
