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
#include "BibMediaPlayer.h" // g_mediaEnabled boot flag
#include "BibPageClients.h"
#include "BibSocketProvider.h"
#include "BibStorage.h"
#include "CommonAtomStrings.h"
#include "CookieJar.h"
#include "CurlContext.h"
#include "CurlRequestScheduler.h" // bib_pump_network -> scheduler().hostPump()
#include "Document.h"
#include "DocumentLoader.h"
#include "DocumentView.h" // inline LocalFrame::view() lives here, not in LocalFrame.h
#include "DocumentWriter.h"
#include "EmptyClients.h"
#include "EventHandler.h"
#include "FocusController.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "GLContext.h" // presentGPU: makeContextCurrent on the shared display's context
#include "GraphicsContextSkia.h"
#include "HandleUserInputEventResult.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "MemoryCache.h"
#include "NetworkStorageSession.h" // persistence: cookieDatabase() dump/seed
#include "Page.h"
#include "PageConfiguration.h"
#include "PlatformDisplay.h"
#include "PlatformDisplayEmscripten.h"
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
#include "DOMWrapperWorld.h"
#include "JSDOMGlobalObject.h"
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSFunction.h>
#include <atomic>
#include <emscripten.h>
#ifdef __EMSCRIPTEN_PTHREADS__
#include <emscripten/proxying.h>
#endif
#include <emscripten/threading.h>
#include <pal/SessionID.h>
#include <wtf/JSONValues.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ProcessPrivilege.h>
#include <wtf/RunLoop.h>
#include <wtf/WallTime.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkPixmap.h>
#include <skia/core/SkSurface.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#include <cstdio>
#include <cstdlib>
#include <utility> // std::exchange

namespace BIB {
void installEmbedderStrategies(); // EmbedderStrategies.cpp
void setRequestBlocklistEnabled(bool); // EmbedderStrategies.cpp
Ref<WebCore::StorageSessionProvider> createEmbedderStorageSessionProvider(); // EmbedderStrategies.cpp
WebCore::NetworkStorageSession& embedderStorageSession(); // EmbedderStrategies.cpp

// Persistence registries declared in BibStorage.h — engine-thread only.
HashMap<String, RefPtr<BibStorageArea>>& bibLocalAreaRegistry()
{
    static NeverDestroyed<HashMap<String, RefPtr<BibStorageArea>>> registry;
    return registry;
}
HashMap<String, HashMap<String, String>>& bibPendingStorageImport()
{
    static NeverDestroyed<HashMap<String, HashMap<String, String>>> pending;
    return pending;
}
}

#ifdef __EMSCRIPTEN_PTHREADS__
// W-B2: runtime-decided OffscreenCanvas transfer for the proxied-main
// (engine) thread. crt1_proxy_main marks its pthread_create with a
// (char*)-1 sentinel meaning "use the -sOFFSCREENCANVASES_TO_PTHREAD link
// list". We deliberately don't link that list: it is compile-time-fixed
// and a canvas missing at spawn fails pthread_create outright — node gates
// have no DOM at all, and raster mode must keep #screen page-owned for its
// 2d context. Intercept the sentinel (-Wl,--wrap=pthread_create,
// embedder.cmake) and substitute the real decision: transfer #screen only
// when the page committed to GPU mode. This also keeps the sentinel away
// from UTF8ToString((char*)-1), which the stock JS would otherwise hit
// with no link list configured.
//
// Runs on the SPAWNING thread — the page main thread for proxied main, so
// EM_ASM reads the page Module directly (node: bibGPU undefined → no
// transfer). All other pthread_creates (thread pool, WTF threads) carry a
// null/zero canvas list and pass through untouched.
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*startRoutine)(void*), void* arg)
{
    if (attr) {
        const char* canvases = nullptr;
        emscripten_pthread_attr_gettransferredcanvases(attr, &canvases);
        if (canvases == reinterpret_cast<const char*>(-1)) {
            pthread_attr_t patched = *attr;
            bool gpu = EM_ASM_INT({ return typeof Module !== "undefined" && Module && Module.bibGPU ? 1 : 0; });
            emscripten_pthread_attr_settransferredcanvases(&patched, gpu ? "#screen" : "");
            return __real_pthread_create(thread, &patched, startRoutine, arg);
        }
    }
    return __real_pthread_create(thread, attr, startRoutine, arg);
}
#endif

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

// Skia GPU (decision-005 G2, opt-in via Module.bibGPU): the backing
// SkSurface in g_engine becomes a Ganesh TEXTURE target, paints stay
// dirty-rect-clipped, and presenting = drawing the backing texture onto a
// wrap of the canvas WebGL2 context's framebuffer 0 (GPU-GPU quad). The
// host page never sees pixels: bib_render returns null and the canvas is
// live. When the flag is unset every byte of the CPU path is unchanged.
static bool g_gpu = false;
static sk_sp<SkSurface> g_fbo0Surface; // present target, GPU mode only
// G4 (W-B2): set by the webglcontextlost handler, cleared after the Ganesh
// world is rebuilt on restore. Atomic: read by bib_render on the engine
// thread, written by canvas event handlers (same thread under pthreads —
// the OffscreenCanvas lives with the engine — but atomic keeps the
// single-threaded build's main-thread handlers honest too).
static std::atomic<bool> g_gpuLost { false };

static void presentGPU()
{
    // Defensive make-current: host-page JS could have bound another GL
    // context since the last frame; Ganesh assumes ITS context is current.
    WebCore::PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent();
    // Full-canvas composite, not dirty-rect: with preserveDrawingBuffer off
    // the FBO 0 content is undefined after each composite, so every present
    // must cover the whole frame (G1-measured at 0.9-1.3ms).
    g_engine->surface->draw(g_fbo0Surface->getCanvas(), 0, 0);
    skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get());
    // Nudge the rendering update: the HTML spec ties OffscreenCanvas
    // placeholder commits to rAF processing in the OWNING scope. Stock
    // Chromium also commits implicitly at task end, but forks/older builds
    // may only present on worker rAF — without this the engine paints at
    // full speed while the SCREEN updates seconds apart (live-fork report,
    // 2026-06-12: bench 2.67ms, guest 94 rAF/s, visible ~1 frame/min).
    // No-op callback (cadence is bib_tick's job); guard collapses presents
    // that outpace the display into one pending rAF.
    EM_ASM({
        if (typeof requestAnimationFrame === "function" && !Module.bibRafNudge) {
            Module.bibRafNudge = true;
            requestAnimationFrame(function() { Module.bibRafNudge = false; });
        }
    });
}

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

// True while view->paint runs (paintFrameRect). A scroll callback arriving
// MID-PAINT would shift pixels under the painter and slip past the frame's
// snapshotted upload box — bibScrollBlit falls back to plain damage instead
// (Codex hypothesis, 2026-06-11).
static bool g_inPaint = false;

// Paint ONLY `dirty` (root-view coords, pre-clamped to the frame, layout
// already up to date — see bib_render) into the persistent surface. Pixels
// outside the clip keep the previous frame's content; g_blitPixels mirrors
// the surface the same way via partial readbacks, so the two stay in sync.
// drawColor, NOT clear(): SkCanvas::clear ignores the clip.
static bool paintFrameRect(const WebCore::IntRect& dirty)
{
    RefPtr view = mainFrameView();
    if (!view)
        return false;
    SkCanvas* canvas = g_engine->surface->getCanvas();
    canvas->save();
    canvas->clipRect(SkRect::MakeXYWH(dirty.x(), dirty.y(), dirty.width(), dirty.height()));
    canvas->drawColor(SK_ColorWHITE);
    // Accelerated mode only changes texture-backed-image handling (no raster
    // copies for shadows) — Canvas-purpose GL gymnastics stay off either way.
    WebCore::GraphicsContextSkia context(*canvas,
        g_gpu ? WebCore::RenderingMode::Accelerated : WebCore::RenderingMode::Unaccelerated,
        WebCore::RenderingPurpose::Unspecified);
    g_inPaint = true;
    view->paint(context, dirty);
    g_inPaint = false;
    canvas->restore();
    return true;
}

// Callers snapshot-and-clear the dirty state BEFORE painting: damage
// reported DURING view->paint (paint-triggered invalidations) must
// accumulate for the NEXT frame, not be wiped post-paint (Codex 2026-06-11).
static void clearDamage()
{
    BIB::g_frameDirty = false;
    BIB::g_damageCount = 0;
    BIB::g_uploadRect = { };
}

// Fast-scroll blit (ChromeClient::scroll): shift the already-painted pixels
// by `delta` within the scrolled clip, in BOTH mirrors (g_blitPixels and the
// SkSurface — they must stay identical or later partial paints composite
// over a stale base). WebCore then only repaints the strips the shift
// exposed; the host re-uploads the whole moved region via g_uploadRect.
static void bibScrollBlit(const WebCore::IntSize& delta, const WebCore::IntRect& rectToScroll, const WebCore::IntRect& clipRect)
{
    const WebCore::IntRect frameRect(0, 0, kWidth, kHeight);
    WebCore::IntRect scrollRect = WebCore::intersection(WebCore::intersection(rectToScroll, clipRect), frameRect);
    if (!g_engine || g_inPaint || scrollRect.isEmpty()) {
        BIB::addDamage(clipRect);
        return;
    }
    const int dx = delta.width(), dy = delta.height();
    WebCore::IntRect dst = scrollRect;
    dst.move(dx, dy);
    dst.intersect(scrollRect);
    if (dst.isEmpty()) { // shifted clean out of the clip — nothing reusable
        BIB::addDamage(scrollRect);
        return;
    }
    WebCore::IntRect src = dst;
    src.move(-dx, -dy);

    // Pending unpainted damage: WebCore invalidates scrollbars/content
    // BEFORE calling ChromeClient::scroll, so bailing out here tripped on
    // EVERY scroll tick and kept blit-shift dormant (2026-06-11 scroll
    // probe: 40/40 full-viewport repaints). Damage tracks the content it
    // marks — translate the part inside the scrolled clip by the delta
    // (the Windows port offsets its backing-store dirty region the same
    // way). Stale pixels can only land where the shift writes (dst);
    // damage shifted beyond dst is overwritten or clipped away with them.
    // Fixed/sticky elements are NOT a smear hazard: scrollContentsFastPath
    // invalidates their old+new rects AFTER this callback returns.
    if (BIB::g_frameDirty) {
        // A pending rect covering the whole scroll region means the blit
        // preserves nothing worth keeping.
        for (size_t i = 0; i < BIB::g_damageCount; ++i) {
            if (BIB::g_damageRects[i].contains(scrollRect)) {
                BIB::addDamage(scrollRect);
                return;
            }
        }
        // Per rect: damage outside the clip stays put; damage straddling
        // the clip edge stays in place in FULL and gains a translated copy
        // (repaints a little extra, never smears); fully-inside damage
        // rides the scroll. Translated copies clip to dst — stale pixels
        // can only land where the shift writes.
        WebCore::IntRect pending[BIB::kMaxDamageRects * 2];
        size_t pendingCount = 0;
        for (size_t i = 0; i < BIB::g_damageCount; ++i) {
            const WebCore::IntRect r = BIB::g_damageRects[i];
            WebCore::IntRect moving = WebCore::intersection(r, scrollRect);
            if (moving.isEmpty()) {
                pending[pendingCount++] = r;
                continue;
            }
            if (moving != r)
                pending[pendingCount++] = r;
            moving.move(dx, dy);
            moving.intersect(dst);
            if (!moving.isEmpty())
                pending[pendingCount++] = moving;
        }
        BIB::g_damageCount = 0;
        BIB::g_frameDirty = false;
        for (size_t i = 0; i < pendingCount; ++i)
            BIB::addDamage(pending[i]); // re-merges + re-arms g_frameDirty
    }

    // Overlap-safe row walk over g_blitPixels (memmove handles x overlap).
    const size_t rowBytes = static_cast<size_t>(dst.width()) * 4;
    if (dy > 0) {
        for (int y = dst.height() - 1; y >= 0; --y)
            memmove(g_blitPixels + ((static_cast<size_t>(dst.y() + y)) * kWidth + dst.x()) * 4,
                g_blitPixels + ((static_cast<size_t>(src.y() + y)) * kWidth + src.x()) * 4, rowBytes);
    } else {
        for (int y = 0; y < dst.height(); ++y)
            memmove(g_blitPixels + ((static_cast<size_t>(dst.y() + y)) * kWidth + dst.x()) * 4,
                g_blitPixels + ((static_cast<size_t>(src.y() + y)) * kWidth + src.x()) * 4, rowBytes);
    }
    // Mirror the shift onto the surface — via the CANVAS writePixels, which
    // (unlike SkSurface::writePixels) reports failure. On failure the two
    // mirrors have DIVERGED (buffer shifted, surface not) — full-frame
    // damage repaints and re-reads everything, resyncing both; never leave
    // it silent (Codex confirmed finding, 2026-06-11).
    auto info = SkImageInfo::Make(dst.width(), dst.height(), kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!g_engine->surface->getCanvas()->writePixels(info, g_blitPixels + (static_cast<size_t>(dst.y()) * kWidth + dst.x()) * 4, kWidth * 4, dst.x(), dst.y())) {
        BIB::addDamage(frameRect);
        return;
    }

    // Host re-uploads everything that moved…
    BIB::g_uploadRect.unite(scrollRect);
    // …WebCore repaints only the exposed strips (scrollRect minus dst):
    // a horizontal band (top or bottom) and/or a vertical band (left/right).
    if (dy > 0)
        BIB::addDamage({ scrollRect.x(), scrollRect.y(), scrollRect.width(), dy });
    else if (dy < 0)
        BIB::addDamage({ scrollRect.x(), scrollRect.maxY() + dy, scrollRect.width(), -dy });
    if (dx > 0)
        BIB::addDamage({ scrollRect.x(), scrollRect.y(), dx, scrollRect.height() });
    else if (dx < 0)
        BIB::addDamage({ scrollRect.maxX() + dx, scrollRect.y(), -dx, scrollRect.height() });
}

// Layout + paint the FULL frame (boot/gate path; bib_render drives the
// dirty-rect path itself so it can snapshot damage after layout).
static bool paintFrame()
{
    if (!mainFrameView())
        return false;
    g_engine->mainFrame->protectedDocument()->updateLayoutIgnorePendingStylesheets();
    clearDamage();
    return paintFrameRect(WebCore::IntRect(0, 0, kWidth, kHeight));
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

// ---------------------------------------------------------------------------
// W-B1 cross-thread entry marshaling. Under -sPROXY_TO_PTHREAD, main() (and
// all of WebCore/JSC) runs on a dedicated pthread, but the host page still
// calls the bib_* exports from the BROWSER MAIN THREAD — a direct call there
// would race the engine. Every export below self-proxies: called on the
// wrong thread, it queues itself onto the engine thread via the emscripten
// system proxying queue (processed whenever the engine pthread returns to
// its event loop — which the event-driven pump guarantees) and returns.
// High-frequency cadence entries (tick/pump) collapse bursts through an
// atomic pending flag so a pegged engine never accumulates a task backlog.

static pthread_t g_engineThread;
static std::atomic<bool> g_engineThreadReady { false };

static bool bibOnEngineThread()
{
    return g_engineThreadReady.load(std::memory_order_acquire)
        && pthread_equal(pthread_self(), g_engineThread);
}

// Queue a task onto the engine thread; drops silently pre-main() (matches
// the !g_engine early-outs every entry point already has).
// BIB_PTHREAD=OFF build: there is only one thread, so after main() runs
// every caller IS the engine thread (bibOnEngineThread() true — emscripten
// stubs pthread_self/pthread_equal) and this path is unreachable; before
// main() it returns false, matching the pthread build's pre-ready drop.
static bool bibProxyToEngine(void (*task)(void*), void* arg)
{
#ifdef __EMSCRIPTEN_PTHREADS__
    if (!g_engineThreadReady.load(std::memory_order_acquire))
        return false;
    return emscripten_proxy_async(emscripten_proxy_get_system_queue(), g_engineThread, task, arg);
#else
    (void)task;
    (void)arg;
    return false;
#endif
}

// Shared with BibMediaPlayer.cpp's bib_media_event export (BibMediaPlayer.h).
namespace BIB {
bool onEngineThread() { return bibOnEngineThread(); }
bool proxyToEngine(void (*task)(void*), void* arg) { return bibProxyToEngine(task, arg); }
}

// ---------------------------------------------------------------------------
// Engine-state persistence: cookies + guest localStorage round-trip through
// the host page's OPFS (web/browser.html). Seed: main() reads
// Module.bibSeedState (one JSON blob) before the first load — cookies go
// straight into the CookieJarDB, localStorage waits in
// bibPendingStorageImport() until each origin's area materializes
// (BibStorage.h). Dump: bib_tick re-serializes every ~5s and pushes to
// Module.bibPersist ONLY when the blob changed (string compare — there is
// no write hook on the cookie jar, so polling is the change detector).
// Everything here runs on the engine thread.

static String bibBuildPersistJSON()
{
    auto root = JSON::Object::create();
    root->setInteger("v"_s, 1);

    auto cookies = JSON::Array::create();
    for (auto& cookie : BIB::embedderStorageSession().cookieDatabase().getAllCookies()) {
        auto c = JSON::Object::create();
        c->setString("name"_s, cookie.name);
        c->setString("value"_s, cookie.value);
        c->setString("domain"_s, cookie.domain);
        c->setString("path"_s, cookie.path);
        if (cookie.expires)
            c->setDouble("expires"_s, *cookie.expires); // ms since epoch
        c->setBoolean("httpOnly"_s, cookie.httpOnly);
        c->setBoolean("secure"_s, cookie.secure);
        c->setBoolean("session"_s, cookie.session);
        cookies->pushObject(WTF::move(c));
    }
    root->setArray("cookies"_s, WTF::move(cookies));

    auto storage = JSON::Object::create();
    for (auto& [origin, area] : BIB::bibLocalAreaRegistry()) {
        auto items = JSON::Object::create();
        area->forEachItem([&](const String& key, const String& value) {
            items->setString(key, value);
        });
        storage->setObject(origin, WTF::move(items));
    }
    // Origins seeded from a previous session that the guest has not opened
    // yet THIS session still live in the pending map — dropping them here
    // would erase a prior session's data just by not visiting the site.
    for (auto& [origin, pendingItems] : BIB::bibPendingStorageImport()) {
        if (BIB::bibLocalAreaRegistry().contains(origin))
            continue;
        auto items = JSON::Object::create();
        for (auto& [key, value] : pendingItems)
            items->setString(key, value);
        storage->setObject(origin, WTF::move(items));
    }
    root->setObject("localStorage"_s, WTF::move(storage));
    return root->toJSONString();
}

static void bibSeedPersistedState(const String& json)
{
    auto value = JSON::Value::parseJSON(json);
    auto root = value ? value->asObject() : nullptr;
    if (!root) {
        printf("EMBEDDER: persist seed unreadable — starting fresh\n");
        return;
    }

    int cookieCount = 0;
    if (auto cookieArray = root->getArray("cookies"_s)) {
        auto& jar = BIB::embedderStorageSession().cookieDatabase();
        double nowMs = WallTime::now().secondsSinceEpoch().milliseconds();
        for (auto& entry : *cookieArray) {
            auto c = entry->asObject();
            if (!c)
                continue;
            WebCore::Cookie cookie;
            cookie.name = c->getString("name"_s);
            cookie.value = c->getString("value"_s);
            cookie.domain = c->getString("domain"_s);
            cookie.path = c->getString("path"_s);
            cookie.expires = c->getDouble("expires"_s);
            cookie.httpOnly = c->getBoolean("httpOnly"_s).value_or(false);
            cookie.secure = c->getBoolean("secure"_s).value_or(false);
            cookie.session = c->getBoolean("session"_s).value_or(false);
            if (cookie.name.isEmpty() && cookie.value.isEmpty())
                continue;
            if (!cookie.session && cookie.expires && *cookie.expires <= nowMs)
                continue; // expired while we were away
            if (jar.setCookie(cookie))
                cookieCount++;
        }
    }

    int originCount = 0;
    if (auto storage = root->getObject("localStorage"_s)) {
        for (auto& member : *storage) {
            auto items = member.value->asObject();
            if (!items)
                continue;
            HashMap<String, String> map;
            for (auto& item : *items) {
                String itemValue;
                if (item.value->asString(itemValue))
                    map.set(item.key, itemValue);
            }
            BIB::bibPendingStorageImport().set(member.key, WTF::move(map));
            originCount++;
        }
    }
    printf("EMBEDDER: persist seed: %d cookies, %d localStorage origins\n", cookieCount, originCount);
}

static void bibMaybePersist(bool force)
{
    static NeverDestroyed<String> lastJSON;
    static MonotonicTime lastCheck;
    static bool oversizeWarned = false;

    if (!g_engine)
        return;
    auto now = MonotonicTime::now();
    if (!force && now - lastCheck < Seconds(5))
        return;
    lastCheck = now;

    String json = bibBuildPersistJSON();
    if (json == lastJSON.get())
        return;
    CString utf8 = json.utf8();
    // localStorage quota (5MB/origin) keeps real blobs far below this; the
    // cap only guards the cross-heap string copy from pathological growth.
    if (utf8.length() > 12 * 1024 * 1024) {
        if (!std::exchange(oversizeWarned, true))
            printf("EMBEDDER: persist blob over 12MB — persistence paused\n");
        return;
    }
    lastJSON.get() = json;

    char* copy = static_cast<char*>(malloc(utf8.length() + 1));
    if (!copy)
        return;
    memcpy(copy, utf8.data(), utf8.length() + 1);
    // Same protocol as bibPushFrameIfDirty: malloc'd payload crosses to the
    // main thread, which reads it out of the (possibly grown) shared heap
    // and frees it via the exported _bib_wasm_free.
    // Module guard: pagehide nulls window.Module while a queued push may
    // still be in flight — free-then-drop is the correct outcome there.
    MAIN_THREAD_ASYNC_EM_ASM({
        if (typeof growMemViews === "function")
            growMemViews();
        var json = UTF8ToString($0);
        _bib_wasm_free($0);
        if (typeof Module !== "undefined" && Module && Module.bibPersist)
            Module.bibPersist(json);
    }, copy);
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int bib_frame_width() { return kWidth; }
EMSCRIPTEN_KEEPALIVE int bib_frame_height() { return kHeight; }

// W-B1 push-model rendering: defined after bib_render (which it drives).
static void bibPushFrameIfDirty();

// One non-blocking engine-RunLoop iteration: fires due WebCore timers and
// dispatched main-thread functions (DOM timers, RenderingUpdateScheduler's
// fallback timer, caret blink). Drive from requestAnimationFrame.
// (RunMode::Iterate never sleeps — RunLoopGeneric's Drain-only waitUntil.)
// W-B1: the host rAF still calls this for CADENCE, but the body runs on the
// engine thread; the frame (if any) is PUSHED back via Module.bibBlit.
static std::atomic<bool> g_tickQueued { false };
static void bibRunTick(void*);
EMSCRIPTEN_KEEPALIVE void bib_tick()
{
    if (!bibOnEngineThread()) {
        if (g_tickQueued.exchange(true, std::memory_order_acq_rel))
            return; // one tick already pending — collapse the burst
        if (!bibProxyToEngine(bibRunTick, nullptr))
            g_tickQueued.store(false, std::memory_order_release);
        return;
    }
    WTF::RunLoop::cycle();
    // Drive WebCore's "update the rendering" steps. This port has no
    // DisplayRefreshMonitor, so nothing else ever runs them — guest
    // requestAnimationFrame callbacks NEVER fired (root cause #16), which
    // silently stalled everything rAF-shaped: CSS/JS animations,
    // IntersectionObserver delivery, and rAF-deferred commits (react-helmet
    // batches <script> head insertions through rAF — 2captcha's reCAPTCHA
    // loader died exactly there). Gated on WebCore actually requesting an
    // update (BibChromeClient::scheduleRenderingUpdate sets the flag and
    // returns true, which also suppresses RenderingUpdateScheduler's
    // fallback timer): one pass per request, max one per host frame, zero
    // on idle pages.
    if (g_engine && std::exchange(BIB::g_renderingUpdateRequested, false)) {
        g_engine->page->updateRendering();
        g_engine->page->finalizeRenderingUpdate({ });
    }
    bibPushFrameIfDirty();
    bibMaybePersist(false);
}
static void bibRunTick(void*)
{
    g_tickQueued.store(false, std::memory_order_release);
    bib_tick();
}

// One engine-RunLoop iteration WITHOUT the rendering-update steps. The
// wake-up plumbing (Module.bibWakeUp -> macrotask, Module.bibArmTimer ->
// setTimeout — WORKER-scope under W-B1, see web/engine-pre.js) calls this,
// so engine work runs at event-loop rate while rendering stays on
// bib_tick's rAF cadence.
static std::atomic<bool> g_pumpQueued { false };
static void bibRunPump(void*);
EMSCRIPTEN_KEEPALIVE void bib_pump()
{
    if (!bibOnEngineThread()) {
        if (g_pumpQueued.exchange(true, std::memory_order_acq_rel))
            return;
        if (!bibProxyToEngine(bibRunPump, nullptr))
            g_pumpQueued.store(false, std::memory_order_release);
        return;
    }
    WTF::RunLoop::cycle();
}
static void bibRunPump(void*)
{
    g_pumpQueued.store(false, std::memory_order_release);
    bib_pump();
}

// Socket-data poke: one curl multi pass right now (the wisp shim calls this
// when WebSocket bytes arrive — SOCKFS can't signal curl), then a RunLoop
// cycle so completions dispatched via callOnMainThread run immediately
// instead of waiting for the next wake-up. The wisp dispatcher lives on the
// browser main thread (W-B0: sockets proxy there), so this entry is ALWAYS
// cross-thread in browser mode.
static std::atomic<bool> g_netPumpQueued { false };
static void bibRunNetPump(void*);
EMSCRIPTEN_KEEPALIVE void bib_pump_network()
{
    if (!bibOnEngineThread()) {
        if (g_netPumpQueued.exchange(true, std::memory_order_acq_rel))
            return;
        if (!bibProxyToEngine(bibRunNetPump, nullptr))
            g_netPumpQueued.store(false, std::memory_order_release);
        return;
    }
    WebCore::CurlContext::singleton().scheduler().hostPump();
    WTF::RunLoop::cycle();
}
static void bibRunNetPump(void*)
{
    g_netPumpQueued.store(false, std::memory_order_release);
    bib_pump_network();
}

// The region bib_render last repainted, as {x, y, w, h} for the host's
// partial putImageData. force=1 and the first frame report the full frame.
static int g_dirtyBox[4] = { 0, 0, kWidth, kHeight };

EMSCRIPTEN_KEEPALIVE const int* bib_dirty_box() { return g_dirtyBox; }

// Returns the RGBA frame buffer (kWidth*kHeight*4) after repainting, or 0.
// force=0: only repaints (and returns the buffer) when the page is dirty —
//          the rAF blit loop skips putImageData on clean frames. Only the
//          accumulated damage union is painted + read back (bib_dirty_box);
//          the rest of g_blitPixels still holds the previous frame, which
//          is exactly what those pixels look like on the surface too.
// force=1: always repaints and returns the FULL frame (pixel probes that
//          sample anywhere in the buffer, first use).
EMSCRIPTEN_KEEPALIVE const uint8_t* bib_render(int force)
{
    // W-B1: engine-thread-only (driven by bib_tick's push; the host no
    // longer pulls frames). A stray main-thread caller gets nullptr.
    if (!bibOnEngineThread())
        return nullptr;
    if (!g_engine)
        return nullptr;
    // G4: while the WebGL context is lost the surfaces are dead/dropped —
    // skip BEFORE the damage snapshot below so nothing is wiped; damage
    // keeps accumulating and the restore path queues a full-frame repaint.
    if (g_gpu && g_gpuLost.load(std::memory_order_acquire))
        return nullptr;
    if (!force && !BIB::g_frameDirty && BIB::g_uploadRect.isEmpty())
        return nullptr;
    RefPtr view = mainFrameView();
    if (!view)
        return nullptr;
    // Layout BEFORE snapshotting the damage union — layout itself reports
    // damage through BibChromeClient, and it must land in THIS frame.
    g_engine->mainFrame->protectedDocument()->updateLayoutIgnorePendingStylesheets();
    const WebCore::IntRect frameRect(0, 0, kWidth, kHeight);
    // Two regions, two meanings: `dirty` = WebCore repaints + we read back;
    // `upload` = pixels bibScrollBlit already shifted in BOTH mirrors — the
    // host just re-uploads them, nothing repaints (a force frame is both).
    WebCore::IntRect dirty[BIB::kMaxDamageRects];
    size_t dirtyCount = 0;
    if (force)
        dirty[dirtyCount++] = frameRect;
    else {
        for (size_t i = 0; i < BIB::g_damageCount; ++i) {
            WebCore::IntRect r = WebCore::intersection(BIB::g_damageRects[i], frameRect);
            if (!r.isEmpty())
                dirty[dirtyCount++] = r;
        }
    }
    WebCore::IntRect upload = force ? WebCore::IntRect() : WebCore::intersection(BIB::g_uploadRect, frameRect);
    clearDamage(); // BEFORE paint — paint-time damage belongs to the next frame
    if (!dirtyCount && upload.isEmpty()) {
        // All damage was outside the viewport — nothing visible changed.
        return nullptr;
    }
    WebCore::IntRect paintedBounds;
    for (size_t i = 0; i < dirtyCount; ++i) {
        const WebCore::IntRect& r = dirty[i];
        bool painted = paintFrameRect(r);
        bool readBack = painted;
        // GPU mode keeps no g_blitPixels mirror — pixels stay on the GPU
        // until presentGPU() composites them to the canvas below.
        if (painted && !g_gpu) {
            auto dstInfo = SkImageInfo::Make(r.width(), r.height(), kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
            uint8_t* dst = g_blitPixels + (static_cast<size_t>(r.y()) * kWidth + r.x()) * 4;
            readBack = g_engine->surface->readPixels(SkPixmap(dstInfo, dst, kWidth * 4), r.x(), r.y());
        }
        if (!painted || !readBack) {
            // Failed paint/readback: re-dirty THIS rect and every rect not
            // yet painted so the next frame retries instead of losing the
            // damage (Codex 2026-06-11). Re-arm the upload region too: it
            // was snapshot-cleared above. (A failed readback leaves the
            // surface newer than g_blitPixels until the retry lands.)
            for (size_t j = i; j < dirtyCount; ++j)
                BIB::addDamage(dirty[j]);
            BIB::g_uploadRect.unite(upload);
            // Rects painted BEFORE the failure are correct in both mirrors
            // but were never reported — without this the host canvas stays
            // stale there until unrelated damage covers it (Codex).
            BIB::g_uploadRect.unite(paintedBounds);
            return nullptr;
        }
        paintedBounds.unite(r);
    }
    WebCore::IntRect box = paintedBounds;
    box.unite(upload);
    g_dirtyBox[0] = box.x();
    g_dirtyBox[1] = box.y();
    g_dirtyBox[2] = box.width();
    g_dirtyBox[3] = box.height();
    if (g_gpu) {
        // The canvas updates when control returns to the event loop
        // (implicit WebGL commit); on clean frames the early-outs above ran
        // and the canvas keeps showing the last presented frame. Null tells
        // the host there is no CPU pixel buffer — it must not putImageData.
        presentGPU();
        return nullptr;
    }
    return g_blitPixels;
}

// W-B1 frame push: render (dirty-rect or forced-first), pack the dirty box
// into a tight malloc'd copy (decoupled from g_blitPixels reuse — the main
// thread consumes asynchronously), and hand it to the page. The receiving
// EM_ASM runs on the browser main thread in module scope: it copies the
// bytes OUT of the shared heap (ImageData rejects SAB-backed views),
// frees the transfer buffer (dlmalloc is thread-safe under -pthread), and
// calls Module.bibBlit. growMemViews() first — views go stale after a
// cross-thread memory grow (W-B0 finding).
static void bibPushFrameIfDirty()
{
    static bool firstFramePushed = false;
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

// Probe/gate path ONLY (G3, decision-005): force a repaint and return CPU
// pixels in BOTH modes. CPU mode is bib_render(1) verbatim; GPU mode adds a
// full-frame Ganesh readback into g_blitPixels — a deliberate GPU sync that
// must never run per-frame (the render loop uses bib_render).
EMSCRIPTEN_KEEPALIVE const uint8_t* bib_render_readback()
{
    if (!bibOnEngineThread())
        return nullptr; // W-B1: probes use bib_request_readback instead
    const uint8_t* ptr = bib_render(1);
    if (!g_gpu)
        return ptr;
    if (!g_engine || !g_engine->surface)
        return nullptr;
    // bib_render(1) painted into the texture surface and presented; read the
    // surface (not FBO 0 — its content is undefined after composite with
    // preserveDrawingBuffer off). presentGPU left our context current, but
    // re-assert defensively like every other GPU entry point.
    WebCore::PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent();
    auto dstInfo = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!g_engine->surface->readPixels(SkPixmap(dstInfo, g_blitPixels, kWidth * 4), 0, 0))
        return nullptr;
    return g_blitPixels;
}

// W-B1 async probe readback: the page cannot pull pixels synchronously
// anymore (a blocking main->engine call risks deadlock against the
// engine's proxied syscalls). The page requests; the engine renders and
// pushes the FULL frame to Module.bibReadbackReady as a copied-out
// Uint8Array. Used by __bib.probe / gate harnesses.
static void bibRunReadback(void*)
{
    const uint8_t* pixels = bib_render_readback();
    uint8_t* copy = nullptr;
    const size_t bytes = static_cast<size_t>(kWidth) * kHeight * 4;
    if (pixels) {
        copy = static_cast<uint8_t*>(malloc(bytes));
        if (copy)
            memcpy(copy, pixels, bytes);
    }
    MAIN_THREAD_ASYNC_EM_ASM({
        var data = null;
        if ($0) {
            if (typeof growMemViews === "function")
                growMemViews();
            data = HEAPU8.slice($0, $0 + $1);
            _bib_wasm_free($0);
        }
        if (Module.bibReadbackReady)
            Module.bibReadbackReady(data, $2, $3);
    }, copy, bytes, kWidth, kHeight);
}
// Collapsed like bib_tick: harness predicates poll probe() every ~100ms —
// while the engine is pegged (Discord boot) the requests must not pile up
// as queued forced repaints. One pending readback serves every waiter
// (bibReadbackReady resolves them all with the same frame).
static std::atomic<bool> g_readbackQueued { false };
static void bibRunReadbackCollapsed(void*)
{
    g_readbackQueued.store(false, std::memory_order_release);
    bibRunReadback(nullptr);
}
EMSCRIPTEN_KEEPALIVE void bib_request_readback()
{
    if (!bibOnEngineThread()) {
        if (g_readbackQueued.exchange(true, std::memory_order_acq_rel))
            return;
        if (!bibProxyToEngine(bibRunReadbackCollapsed, nullptr))
            g_readbackQueued.store(false, std::memory_order_release);
        return;
    }
    bibRunReadback(nullptr);
}

// --- G4: in-place GPU context-loss recovery (W-B2) ----------------------
// The browser can revoke a WebGL context at any time (GPU reset, driver
// restart, resource pressure). G3 recovered by reloading the page; G4
// recovers IN PLACE: the lost handler preventDefault()s (required, or the
// browser never restores) and parks rendering (bib_render's g_gpuLost
// guard); the restored handler rebuilds the Ganesh world over the SAME
// context handle (WebGL restores the same context object, so the GLContext
// facades and the assembled proc table stay valid) and queues a full
// repaint. If restored never fires (some drivers won't), a deadline falls
// back to the G3 protocol: the page reloads (?gpulost=1, second loss
// ?gpu=0). Guest ImageBuffers from accelerated 2D canvases (?canvasgpu)
// are NOT rebuilt — accepted v1 gap, they re-create on guest redraw paths.

extern "C" void bibEnableAllWebGLExtensions(); // EM_JS, PlatformDisplayEmscripten.cpp

static void bibGpuRebuildAfterRestore()
{
    auto& display = WebCore::PlatformDisplay::sharedDisplay();
    // Restoration invalidated every previously enabled extension (WebGL
    // spec) — re-enable BEFORE Skia re-reads caps below, or shaders using
    // advertised extensions fail to compile again. Make the (restored)
    // context current first: the EM_JS helper reads GL.currentContext.
    emscripten_webgl_make_context_current(WebCore::emscriptenSkiaWebGLContext());
    bibEnableAllWebGLExtensions();
    // Documented teardown order for a lost context: abandon the GrContext
    // FIRST (destruction would otherwise drive dead GL), then drop the
    // surfaces that reference it.
    if (auto* grContext = display.skiaGrContext())
        grContext->abandonContext();
    g_fbo0Surface = nullptr;
    if (g_engine)
        g_engine->surface = nullptr;
    // Upstream hook (public wrapper over the private clearSkiaGLContext):
    // clears the thread-local SkiaGLContext; the next skiaGLContext() call
    // builds a fresh GrDirectContext over the restored WebGL handle via
    // GLContext::createOffscreen + the same proc table. Also nulls
    // m_sharingGLContext, which this port never creates — harmless.
    display.clearGLContexts();
    bool ok = display.skiaGLContext() && display.skiaGrContext();
    if (ok && g_engine) {
        // Same surface pair as the boot path in main(): texture-backed
        // store + FBO 0 present wrap (samples=1/stencil=8 match the
        // context attributes).
        auto info = SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        auto* grContext = display.skiaGrContext();
        g_engine->surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kNo, info, 0, kTopLeft_GrSurfaceOrigin, nullptr);
        if (g_engine->surface) {
            GrGLFramebufferInfo fbInfo;
            fbInfo.fFBOID = 0;
            fbInfo.fFormat = 0x8058; // GL_RGBA8
            auto target = GrBackendRenderTargets::MakeGL(kWidth, kHeight, 1, 8, fbInfo);
            g_fbo0Surface = SkSurfaces::WrapBackendRenderTarget(grContext, target,
                kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
        }
        ok = g_engine->surface && g_fbo0Surface;
    }
    if (!ok) {
        // Same split-brain hazard as a failed GPU boot: the host has no
        // blit path, so CPU raster would never reach the screen. Reload.
        printf("EMBEDDER: gpu restore REBUILD FAILED — host reload\n");
        MAIN_THREAD_ASYNC_EM_ASM({ if (Module.bibGpuLostReload) Module.bibGpuLostReload(); });
        return;
    }
    g_gpuLost.store(false, std::memory_order_release);
    printf("EMBEDDER: gpu context restored\n");
    // Everything on the old context is gone — full-frame repaint. The next
    // bib_tick's push renders it.
    BIB::addDamage(WebCore::IntRect(0, 0, kWidth, kHeight));
}

extern "C" {
// Called directly from the canvas event handlers below — they run on the
// thread that owns the canvas, which is the engine thread (pthread build:
// the OffscreenCanvas lives here; single-thread build: main IS the engine).
EMSCRIPTEN_KEEPALIVE void bib_gpu_context_lost()
{
    // No g_gpu guard: the handlers are installed right after context
    // creation, BEFORE the caps stage sets g_gpu (Codex MEDIUM) — a
    // mid-boot loss must still be flagged.
    g_gpuLost.store(true, std::memory_order_release);
    printf("EMBEDDER: gpu context LOST — awaiting restore\n");
}

EMSCRIPTEN_KEEPALIVE void bib_gpu_context_restored()
{
    if (!g_gpuLost.load(std::memory_order_acquire))
        return;
    if (!g_gpu) {
        // Lost+restored entirely within the boot window: nothing to
        // rebuild yet — boot either succeeded on the restored context or
        // failed into the host raster-reload fallback.
        g_gpuLost.store(false, std::memory_order_release);
        return;
    }
    bibGpuRebuildAfterRestore();
}

EMSCRIPTEN_KEEPALIVE void bib_gpu_restore_timed_out()
{
    if (!g_gpuLost.load(std::memory_order_acquire))
        return;
    printf("EMBEDDER: gpu restore TIMED OUT — host reload\n");
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.bibGpuLostReload) Module.bibGpuLostReload(); });
}
}

// Registered from main() after a successful GPU boot. Worker scope
// (pthread build) sees the transferred OffscreenCanvas in
// GL.offscreenCanvases; the single-threaded build falls back to the DOM
// canvas. Both are EventTargets firing webglcontextlost/restored. The
// handlers run between engine tasks (worker/main event loop), never
// mid-paint, so calling the exports directly is safe.
static void installGpuContextLossHandlers()
{
    EM_ASM({
        var entry = typeof GL !== "undefined" && GL.offscreenCanvases && GL.offscreenCanvases["screen"];
        var canvas = entry ? entry.offscreenCanvas
            : (typeof document !== "undefined" ? document.querySelector("#screen") : null);
        if (!canvas) {
            out("EMBEDDER: gpu loss handlers NOT installed (no canvas handle)");
            return;
        }
        var deadline = null;
        canvas.addEventListener("webglcontextlost", function(e) {
            e.preventDefault(); // REQUIRED or the browser never restores
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
}

// Test hook (gate8): lose the live context via WEBGL_lose_context, restore
// it 500ms later — exercises the full G4 path. Self-proxies like every
// page-callable export.
static void bibRunGpuTestLose(void*)
{
    if (!g_gpu || g_gpuLost.load(std::memory_order_acquire))
        return;
    auto* glContext = WebCore::PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!glContext || !glContext->makeContextCurrent())
        return;
    EM_ASM({
        var ext = GL.currentContext && GL.currentContext.GLctx.getExtension("WEBGL_lose_context");
        if (!ext) {
            out("EMBEDDER: WEBGL_lose_context unavailable");
            return;
        }
        ext.loseContext();
        setTimeout(function() { ext.restoreContext(); }, 500);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE void bib_gpu_test_lose_restore()
{
    if (!bibOnEngineThread()) {
        bibProxyToEngine(bibRunGpuTestLose, nullptr);
        return;
    }
    bibRunGpuTestLose(nullptr);
}

// --- Input forwarding (canvas events -> WebCore EventHandler) ---
// W-B1: each entry self-proxies with a heap-copied argument pack; events
// run on the engine thread in arrival order (single FIFO proxy queue).

struct BibMouseMoveTask { double x; double y; int mods; };
static void bibRunMouseMove(void*);
EMSCRIPTEN_KEEPALIVE void bib_mouse_move(double x, double y, int modifierBits)
{
    if (!bibOnEngineThread()) {
        auto* task = new BibMouseMoveTask { x, y, modifierBits };
        if (!bibProxyToEngine(bibRunMouseMove, task))
            delete task; // pre-main: drop, matches !g_engine
        return;
    }
    if (!g_engine)
        return;
    WebCore::PlatformMouseEvent event({ x, y }, { x, y }, WebCore::MouseButton::None,
        WebCore::PlatformEvent::Type::MouseMoved, 0, modifiersFromBits(modifierBits),
        MonotonicTime::now(), 0, WebCore::SyntheticClickType::NoTap);
    g_engine->mainFrame->eventHandler().mouseMoved(event);
}
static void bibRunMouseMove(void* p)
{
    auto* t = static_cast<BibMouseMoveTask*>(p);
    bib_mouse_move(t->x, t->y, t->mods);
    delete t;
}

struct BibMouseButtonTask { int down; int jsButton; double x; double y; int clicks; int mods; };
static void bibRunMouseButton(void*);
EMSCRIPTEN_KEEPALIVE void bib_mouse_button(int down, int jsButton, double x, double y, int clickCount, int modifierBits)
{
    if (!bibOnEngineThread()) {
        auto* task = new BibMouseButtonTask { down, jsButton, x, y, clickCount, modifierBits };
        if (!bibProxyToEngine(bibRunMouseButton, task))
            delete task;
        return;
    }
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
static void bibRunMouseButton(void* p)
{
    auto* t = static_cast<BibMouseButtonTask*>(p);
    bib_mouse_button(t->down, t->jsButton, t->x, t->y, t->clicks, t->mods);
    delete t;
}

struct BibWheelTask { double x; double y; double dx; double dy; int mods; };
static void bibRunWheel(void*);
EMSCRIPTEN_KEEPALIVE void bib_wheel(double x, double y, double deltaX, double deltaY, int modifierBits)
{
    if (!bibOnEngineThread()) {
        auto* task = new BibWheelTask { x, y, deltaX, deltaY, modifierBits };
        if (!bibProxyToEngine(bibRunWheel, task))
            delete task;
        return;
    }
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
static void bibRunWheel(void* p)
{
    auto* t = static_cast<BibWheelTask*>(p);
    bib_wheel(t->x, t->y, t->dx, t->dy, t->mods);
    delete t;
}

// Diagnostics: dump script/scroll state to stdout (host console).
static void bibRunDiag(void*);
EMSCRIPTEN_KEEPALIVE void bib_diag()
{
    if (!bibOnEngineThread()) {
        bibProxyToEngine(bibRunDiag, nullptr);
        return;
    }
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
static void bibRunDiag(void*) { bib_diag(); }

// Diagnostics: programmatic scroll, bypassing the wheel-event path.
static void bibRunScrollTo(void*);
EMSCRIPTEN_KEEPALIVE void bib_scroll_to(int y)
{
    if (!bibOnEngineThread()) {
        bibProxyToEngine(bibRunScrollTo, reinterpret_cast<void*>(static_cast<intptr_t>(y)));
        return;
    }
    if (!g_engine)
        return;
    if (RefPtr view = mainFrameView())
        view->setScrollPosition(WebCore::ScrollPosition(0, y));
}
static void bibRunScrollTo(void* p) { bib_scroll_to(static_cast<int>(reinterpret_cast<intptr_t>(p))); }

// Diagnostic: run a script string in the guest main world. Output channel
// is the guest console (wrap probes in console.log(...) — the forwarder
// puts them on stderr as "BIB: console log: ..."). Returns 0 if the engine
// is not up. Drives DOM/computed-style probes that are otherwise
// impossible from the host side.
// W-B1: proxied cross-thread; the optimistic `1` only means "queued" there
// (results travel via the guest-console forwarder anyway).
static void bibRunEval(void*);
EMSCRIPTEN_KEEPALIVE int bib_eval(const char* source)
{
    if (!bibOnEngineThread()) {
        if (!source)
            return 0;
        char* copy = strdup(source);
        if (!bibProxyToEngine(bibRunEval, copy)) {
            free(copy);
            return 0;
        }
        return 1;
    }
    if (!g_engine || !source)
        return 0;
    g_engine->mainFrame->script().executeScriptIgnoringException(String::fromUTF8(source), JSC::SourceTaintedOrigin::Untainted);
    return 1;
}
static void bibRunEval(void* p)
{
    char* source = static_cast<char*>(p);
    bib_eval(source);
    free(source);
}

// Host-triggered persistence flush (visibilitychange/pagehide): skips the
// 5s throttle but still skips the push when nothing changed.
static void bibRunPersistNow(void*);
EMSCRIPTEN_KEEPALIVE void bib_persist_now()
{
    if (!bibOnEngineThread()) {
        bibProxyToEngine(bibRunPersistNow, nullptr);
        return;
    }
    bibMaybePersist(true);
}
static void bibRunPersistNow(void*) { bib_persist_now(); }

// ---------------------------------------------------------------------------
// Guest WebAssembly shim (decision-006 S-A). JSC's wasm tiers are all
// unavailable on the CLoop port (IPInt has no cloop lowering — NO-GO), so
// guest pages get a polyfill instead: the host page translates module bytes
// to plain JS with Binaryen's wasm2js and the polyfill evals the result.
// Engine side is two dumb pipes, both installed per new window object by
// BibFrameLoaderClient::dispatchDidClearWindowObjectInWorld:
//   1. __bibWasm2js(payload, mode) — native fn on the guest global; ships a
//      base64 module to Module.bibWasm2js in the host realm (same thread:
//      the whole stack is single-threaded, EM_ASM is a synchronous call into
//      the page realm) and returns the host's string, or null.
//   2. the polyfill source (web/wasm-polyfill.js) — provided by the host as
//      Module.bibWasmPolyfill, evaluated before any author script. Hosts
//      that define neither (node gate runner) keep today's no-wasm behavior.

EMSCRIPTEN_KEEPALIVE char* bib_wasm_alloc(int size)
{
    // EM_ASM blocks below allocate engine-heap buffers for returned strings;
    // malloc itself is not in the JS export set, KEEPALIVE exports are.
    return static_cast<char*>(malloc(size));
}

EMSCRIPTEN_KEEPALIVE void bib_wasm_free(char* ptr)
{
    // W-B1: the frame-push / readback EM_ASM blocks free their transfer
    // buffers FROM THE MAIN THREAD — dlmalloc is thread-safe under -pthread.
    free(ptr);
}

// The JSC host function and the WTF/JSC-typed helpers below need C++
// linkage — step outside the export block (reopened after them).
} // extern "C"

JSC_DEFINE_HOST_FUNCTION(bibWasm2jsHostFunction, (JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame))
{
    JSC::VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String payload = callFrame->argument(0).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String mode = callFrame->argument(1).toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    CString payloadUTF8 = payload.utf8();
    CString modeUTF8 = mode.utf8();
    char* out = static_cast<char*>(EM_ASM_PTR({
        let result = null;
        try {
            if (Module.bibWasm2js)
                result = Module.bibWasm2js(UTF8ToString($0), UTF8ToString($1));
        } catch (e) {
            (Module.printErr || console.error)("bibWasm2js host error: " + e);
        }
        if (typeof result !== "string")
            return 0;
        const len = lengthBytesUTF8(result) + 1;
        const buf = _bib_wasm_alloc(len);
        // A large translatable module (Discord ships several MB-scale ones)
        // can produce a multi-MB JS string; under wasm32 heap pressure the
        // alloc can fail. Without this guard stringToUTF8 writes through a
        // null pointer -> "memory access out of bounds" (Codex Rank 2).
        if (!buf)
            return 0;
        stringToUTF8(result, buf, len);
        return buf;
    }, payloadUTF8.data(), modeUTF8.data()));
    if (!out)
        return JSC::JSValue::encode(JSC::jsNull());
    JSC::JSString* result = JSC::jsString(vm, String::fromUTF8(out));
    free(out);
    return JSC::JSValue::encode(result);
}

static const String& wasmPolyfillSource()
{
    static NeverDestroyed<String> source = [] {
        char* text = static_cast<char*>(EM_ASM_PTR({
            const text = Module.bibWasmPolyfill;
            if (typeof text !== "string" || !text.length)
                return 0;
            const len = lengthBytesUTF8(text) + 1;
            const buf = _bib_wasm_alloc(len);
            stringToUTF8(text, buf, len);
            return buf;
        }));
        if (!text)
            return String();
        String result = String::fromUTF8(text);
        free(text);
        return result;
    }();
    return source;
}

void BIB::injectWasmPolyfill(WebCore::LocalFrame& frame, WebCore::DOMWrapperWorld& world)
{
    if (&world != &WebCore::mainThreadNormalWorldSingleton())
        return;
    const String& polyfill = wasmPolyfillSource();
    if (polyfill.isEmpty())
        return;
    auto* globalObject = frame.script().globalObject(world);
    if (!globalObject)
        return;
    JSC::VM& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    globalObject->putDirect(vm, JSC::Identifier::fromString(vm, "__bibWasm2js"_s),
        JSC::JSFunction::create(vm, globalObject, 2, "__bibWasm2js"_s, bibWasm2jsHostFunction, JSC::ImplementationVisibility::Public),
        static_cast<unsigned>(JSC::PropertyAttribute::DontEnum));
    // The polyfill consumes (and deletes) __bibWasm2js from the global, then
    // defines WebAssembly. Runs before any author script in this document.
    frame.script().executeScriptIgnoringException(String { polyfill }, JSC::SourceTaintedOrigin::Untainted);
}

extern "C" {

// Phase 4: real navigation. Drives FrameLoader::load -> DocumentLoader ->
// CachedResourceLoader -> EmbedderLoaderStrategy -> CurlRequest -> Wisp.
static void bibRunLoadUrl(void*);
EMSCRIPTEN_KEEPALIVE void bib_load_url(const char* url)
{
    if (!bibOnEngineThread()) {
        if (!url)
            return;
        char* copy = strdup(url);
        if (!bibProxyToEngine(bibRunLoadUrl, copy))
            free(copy);
        return;
    }
    if (!g_engine)
        return;
    WebCore::ResourceRequest request { URL { String::fromUTF8(url) } };
    WebCore::FrameLoadRequest frameLoadRequest { *g_engine->mainFrame, WTF::move(request) };
    printf("EMBEDDER: loading %s\n", url);
    g_engine->mainFrame->loader().load(WTF::move(frameLoadRequest));
}
static void bibRunLoadUrl(void* p)
{
    char* url = static_cast<char*>(p);
    bib_load_url(url);
    free(url);
}

// type: 0 = RawKeyDown, 1 = KeyUp, 2 = Char. Strings are UTF-8 (use ccall).
// text is only meaningful for Char events.
// W-B1: cross-thread returns 1 optimistically — the engine-side handled
// verdict isn't knowable without a blocking round-trip (deadlock-prone).
// The host only consumes the return for preventDefault, where over-eager
// handling is the safe direction.
struct BibKeyTask { int type; char* key; char* code; char* text; int vk; int repeat; int mods; };
static void bibRunKey(void*);
EMSCRIPTEN_KEEPALIVE int bib_key(int type, const char* key, const char* code, const char* text, int windowsVirtualKeyCode, int isAutoRepeat, int modifierBits)
{
    if (!bibOnEngineThread()) {
        auto* task = new BibKeyTask { type, strdup(key ? key : ""), strdup(code ? code : ""),
            strdup(text ? text : ""), windowsVirtualKeyCode, isAutoRepeat, modifierBits };
        if (!bibProxyToEngine(bibRunKey, task)) {
            free(task->key);
            free(task->code);
            free(task->text);
            delete task;
            return 0;
        }
        return 1;
    }
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
static void bibRunKey(void* p)
{
    auto* t = static_cast<BibKeyTask*>(p);
    bib_key(t->type, t->key, t->code, t->text, t->vk, t->repeat, t->mods);
    free(t->key);
    free(t->code);
    free(t->text);
    delete t;
}

} // extern "C"

int main()
{
    // W-B1: record the engine thread FIRST — every export's self-proxy
    // check needs it, and the host page starts calling exports the moment
    // onEngineReady fires.
    g_engineThread = pthread_self();
    g_engineThreadReady.store(true, std::memory_order_release);

    // Install the worker-scope Module hooks (pump, bibWasmPolyfill,
    // bibWasm2js — web/engine-pre.js) NOW: this EM_ASM runs in the engine
    // pthread's worker scope with Module fully constructed. The pre-js's
    // own eager attempts can fire before the pthread bootstrap builds
    // Module (gate9: empty bibWasmPolyfill got cached for the session).
    EM_ASM({
        if (typeof self !== "undefined" && self.__bibInstallWorkerHooks)
            self.__bibInstallWorkerHooks();
    });
    printf("EMBEDDER: engine thread=%p browser-main=%d\n",
        reinterpret_cast<void*>(g_engineThread), emscripten_is_main_browser_thread());
    // Verbose libcurl tracing (?curldebug=1 on the host page). Set from C
    // because Module.ENV-based getenv proved unreliable here. MUST happen
    // before installEmbedderStrategies(): the cookie-session setup
    // constructs the CurlContext singleton, which reads DEBUG_CURL exactly
    // once in its constructor.
    // W-B1: all boot flags live on the PAGE's Module — the engine pthread's
    // worker Module does not inherit them (W-B0 finding). MAIN_THREAD_EM_ASM
    // blocks this thread briefly while the main thread answers; safe at
    // boot, before the page starts driving us.
    if (MAIN_THREAD_EM_ASM_INT({ return Module.bibCurlDebug ? 1 : 0; }))
        setenv("DEBUG_CURL", "1", 1);
    printf("EMBEDDER: curldebug=%s\n", getenv("DEBUG_CURL") ? "on" : "off");

    // Request blocklist (?noblock=1 on the host page disables it): the loader
    // strategy refuses analytics/ads/telemetry subresources before they
    // download — CLoop parses every script byte on the main thread, so those
    // bundles are pure boot cost.
    bool noBlock = MAIN_THREAD_EM_ASM_INT({ return Module.bibNoBlock ? 1 : 0; });
    BIB::setRequestBlocklistEnabled(!noBlock);
    printf("EMBEDDER: request blocklist=%s\n", noBlock ? "off" : "on");
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

    // Event-driven pump: every main-RunLoop wake-up (dispatch, timer start)
    // pokes the host page, which schedules a macrotask calling bib_pump().
    // Without this the engine only made progress once per host display
    // frame (rAF -> bib_tick), quantizing EVERY async hop — curl passes,
    // callOnMainThread chains, setTimeout(0) — to ~16.7ms each (measured:
    // 13.9ms per setTimeout(0) hop, 93ms for a warm same-origin fetch).
    // The callback fires while the RunLoop lock is held: bibWakeUp must
    // only schedule, never call back into the engine synchronously.
    // W-B1: this EM_ASM executes in the ENGINE pthread's worker scope —
    // Module.bibWakeUp there is installed by web/engine-pre.js (worker-local
    // MessageChannel calling _bib_pump on this same thread), NOT the page's.
    WTF::RunLoop::setWakeUpCallback([] {
        EM_ASM({ if (Module.bibWakeUp) Module.bibWakeUp(); });
    });

    const bool interactive = MAIN_THREAD_EM_ASM_INT({ return Module.bibInteractive ? 1 : 0; });

    // M-A media bridge gate (?media=1): must be set before the first
    // MediaPlayer construction — buildMediaEnginesVector runs once and
    // caches for the session.
    BIB::g_mediaEnabled = interactive && MAIN_THREAD_EM_ASM_INT({ return Module.bibMedia ? 1 : 0; });
    if (BIB::g_mediaEnabled)
        printf("EMBEDDER: media bridge ENABLED (audio-only, wisp-routed fetch)\n");

    // Skia GPU boot (decision-005 G2, opt-in via Module.bibGPU / ?gpu=1).
    // Must run before ANY paint: GraphicsContextSkia consults the shared
    // PlatformDisplay. W-B2: under PROXY_TO_PTHREAD the page's #screen
    // arrives on THIS thread as an OffscreenCanvas (transferred at spawn by
    // __wrap_pthread_create) and emscripten_webgl_create_context("#screen")
    // resolves it through GL.offscreenCanvases; the single-threaded build
    // creates the context on the page canvas directly (pre-W-B1 path).
    // With the flag unset nothing here runs — the CPU path is unchanged.
    if (interactive && MAIN_THREAD_EM_ASM_INT({ return Module.bibGPU ? 1 : 0; })) {
        if (WebCore::initializePlatformDisplayEmscripten("#screen")) {
            // G4 handlers go on FIRST (Codex MEDIUM): a loss between
            // context creation and handler install would miss
            // preventDefault and be unrestorable. A loss DURING the caps
            // setup below now flags g_gpuLost; the boot then either fails
            // here (GrContext caps against a lost context) → host raster
            // reload, or the restored handler rebuilds post-boot.
            installGpuContextLossHandlers();
            // Force SkiaGLContext creation now; a null GrContext means the
            // interface/caps stage failed — fall back to CPU raster.
            auto& display = WebCore::PlatformDisplay::sharedDisplay();
            g_gpu = display.skiaGLContext() && display.skiaGrContext();
        }
        printf("EMBEDDER: gpu=%s\n", g_gpu ? "on" : "REQUESTED-BUT-UNAVAILABLE (cpu fallback)");
        if (!g_gpu) {
            // The host committed to GPU mode (no 2d context, ignores frame
            // pushes) before the engine could fail — an engine-only
            // fallback would leave the canvas permanently blank (Codex
            // HIGH, G3). Tell the host so it can reload with ?gpu=0.
            MAIN_THREAD_ASYNC_EM_ASM({ if (Module.bibGpuFallback) Module.bibGpuFallback(); });
        }
    }

    printf("EMBEDDER: init OK\n");

    // PageIdentifier: the empty-clients recipe (SVGImage) passes nullopt,
    // but Page::mediaSessionManager() hard-returns null for identifier-less
    // pages — playInternal() then parks every <audio>/<video> play() forever
    // ("returning because of interruption", M-A root cause). Real pages
    // always carry one; generating it also unlocks the default
    // PlatformMediaSessionManager factory.
    auto pageConfiguration = WebCore::pageConfigurationWithEmptyClients(WebCore::PageIdentifier::generate(), PAL::SessionID::defaultSessionID());
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
    // Fail-fast WebSocket provider (WS-0): the empty-clients SocketProvider
    // returns a null channel and WebSocket::create RELEASE_ASSERTs on it —
    // any guest `new WebSocket()` aborted the engine (discord.com/login dies
    // on its remote-auth gateway socket). This one fails the connection like
    // an unreachable server (error event + close 1006) instead. WS-1 (real
    // channel over curl-ws) replaces the channel, not this wiring.
    pageConfiguration.socketProvider = BIB::BibSocketProvider::create();

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
    // Raw-WebCore MemoryCache default is 8MB total — one modern page evicts
    // everything, so every in-engine navigation refetched all subresources
    // over wisp. Still in-memory; sized like a small browser profile.
    WebCore::MemoryCache::singleton().setCapacities(0, 16 * 1024 * 1024, 64 * 1024 * 1024);

    // Persistence seed (cookies + guest localStorage from a previous host
    // session, loaded out of OPFS by browser.html). Must land before the
    // first network request so the initial navigation already attaches the
    // restored cookies. Same SAB string-copy recipe as bibHTML below.
    {
        int seedBytes = MAIN_THREAD_EM_ASM_INT({ return Module.bibSeedState ? lengthBytesUTF8(Module.bibSeedState) + 1 : 0; });
        if (seedBytes > 0) {
            char* seedJSON = static_cast<char*>(malloc(seedBytes));
            if (seedJSON) {
                MAIN_THREAD_EM_ASM({ stringToUTF8(Module.bibSeedState, $0, $1); }, seedJSON, seedBytes);
                bibSeedPersistedState(String::fromUTF8(seedJSON));
                free(seedJSON);
            }
        }
    }

    RefPtr localMainFrame = page->localMainFrame();
    if (!localMainFrame) {
        printf("EMBEDDER: FAIL no localMainFrame\n");
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
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
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
    }

    // The host page may supply the document via Module.bibHTML (a JS
    // string); without it the built-in gate page loads. Copied out of the
    // JS heap via stringToUTF8 (forced into the runtime by
    // EXPORTED_RUNTIME_METHODS) into a malloc'd UTF-8 buffer.
    char* hostHTML = nullptr;
    int hostHTMLBytes = MAIN_THREAD_EM_ASM_INT({ return Module.bibHTML ? lengthBytesUTF8(Module.bibHTML) + 1 : 0; });
    if (hostHTMLBytes > 0) {
        hostHTML = static_cast<char*>(malloc(hostHTMLBytes));
        if (hostHTML)
            // Sync proxy: stringToUTF8 runs on the main thread, writing
            // through its view into the SHARED heap — visible here on return.
            MAIN_THREAD_EM_ASM({ stringToUTF8(Module.bibHTML, $0, $1); }, hostHTML, hostHTMLBytes);
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
    sk_sp<SkSurface> surface;
    if (g_gpu) {
        // Texture-backed backing store (NOT framebuffer 0 directly: with
        // preserveDrawingBuffer off, FBO 0 is undefined after every
        // composite — dirty-rect painting needs stable pixels). Present
        // wraps FBO 0 once; samples=1/stencil=8 match the context attrs
        // (G1: GL_SAMPLES=0 with antialias:false, stencil honored at 8).
        auto* grContext = WebCore::PlatformDisplay::sharedDisplay().skiaGrContext();
        surface = SkSurfaces::RenderTarget(grContext, skgpu::Budgeted::kNo, info, 0, kTopLeft_GrSurfaceOrigin, nullptr);
        if (surface) {
            GrGLFramebufferInfo fbInfo;
            fbInfo.fFBOID = 0;
            fbInfo.fFormat = 0x8058; // GL_RGBA8
            auto target = GrBackendRenderTargets::MakeGL(kWidth, kHeight, 1, 8, fbInfo);
            g_fbo0Surface = SkSurfaces::WrapBackendRenderTarget(grContext, target,
                kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
        }
        if (!surface || !g_fbo0Surface) {
            printf("EMBEDDER: gpu surface setup failed — cpu fallback\n");
            g_fbo0Surface = nullptr;
            surface = nullptr;
            g_gpu = false;
            // Same split-brain hazard as the boot-init fallback above: the
            // host is in GPU mode with no blit path — engine CPU raster
            // would never reach the screen (Codex HIGH, G3).
            MAIN_THREAD_ASYNC_EM_ASM({ if (Module.bibGpuFallback) Module.bibGpuFallback(); });
        }
        if (g_gpu) {
            // W-B2 software-renderer guard: shielded/forked browsers can
            // hand a worker OffscreenCanvas a SOFTWARE WebGL device
            // (SwiftShader/llvmpipe) — and the same browsers MASK the
            // renderer string, so don't trust it: MEASURE. Three full
            // presents with a 1x1 readback sync: real GPUs run ~1-3ms per
            // frame (G1: 0.9-1.3ms); software takes tens of ms (live
            // report: ~1 frame/MINUTE on a Chromium fork). Slow → raster,
            // which IS the better engine on that hardware.
            char rendererBuf[128] = { 0 };
            EM_ASM({
                var ctx = GL.currentContext && GL.currentContext.GLctx;
                var s = "";
                if (ctx) {
                    var ext = ctx.getExtension("WEBGL_debug_renderer_info");
                    s = String((ext ? ctx.getParameter(ext.UNMASKED_RENDERER_WEBGL) : ctx.getParameter(ctx.RENDERER)) || "");
                }
                stringToUTF8(s.slice(0, 120), $0, 127);
            }, rendererBuf);
            uint32_t benchPixel = 0;
            auto benchInfo = SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
            // WARMUP, not measured: the first frames on a fresh context pay
            // one-time pipeline/shader compiles (G1: frame0 ≈ 100ms on real
            // hardware) — measuring them reads a healthy GPU as "software"
            // (observed: Intel UHD 630 "26.67ms/frame" on first-3-frames).
            // Software renderers stay slow on EVERY frame; that's the
            // discriminator, so bench only post-warmup steady state.
            // 8x overdraw per frame: a single fill+quad is so simple that
            // SwiftShader steady-states at ~5ms (measured) — too close to
            // real GPUs for a safe threshold. Overdraw multiplies software
            // raster cost (~40ms) while hardware barely notices (~1ms).
            for (int i = 0; i < 2; ++i) {
                surface->getCanvas()->drawColor(SK_ColorWHITE);
                for (int j = 0; j < 8; ++j)
                    surface->draw(g_fbo0Surface->getCanvas(), 0, 0);
                skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get());
            }
            g_fbo0Surface->readPixels(SkPixmap(benchInfo, &benchPixel, 4), 0, 0);
            auto benchStart = MonotonicTime::now();
            for (int i = 0; i < 3; ++i) {
                surface->getCanvas()->drawColor(SK_ColorWHITE);
                for (int j = 0; j < 8; ++j)
                    surface->draw(g_fbo0Surface->getCanvas(), 0, 0);
                skgpu::ganesh::FlushAndSubmit(g_fbo0Surface.get());
            }
            // The readback is the sync: WebGL queues are deep and flush is
            // advisory — readPixels forces completion everywhere.
            g_fbo0Surface->readPixels(SkPixmap(benchInfo, &benchPixel, 4), 0, 0);
            double msPerFrame = (MonotonicTime::now() - benchStart).milliseconds() / 3.0;
            printf("EMBEDDER: gpu renderer \"%s\" present-bench %.2fms/frame\n", rendererBuf, msPerFrame);
            // ?gpubench=0 (gate8): measure but don't enforce — playwright
            // headless IS SwiftShader, where the pipeline is correct but
            // raster is the faster engine.
            bool enforceBench = MAIN_THREAD_EM_ASM_INT({ return Module.bibGpuBench === false ? 0 : 1; });
            if (enforceBench && msPerFrame > 12.0) {
                printf("EMBEDDER: gpu looks SOFTWARE-RENDERED — cpu fallback\n");
                g_fbo0Surface = nullptr;
                surface = nullptr;
                g_gpu = false;
                MAIN_THREAD_ASYNC_EM_ASM({ if (Module.bibGpuFallback) Module.bibGpuFallback(); });
            }
        }
    }
    if (!g_gpu)
        surface = SkSurfaces::Raster(info);
    if (!surface) {
        printf("EMBEDDER: FAIL SkSurface\n");
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
    }

    g_engine = new Engine { WTF::move(page), WTF::move(localMainFrame), WTF::move(surface) };
    // Fast-scroll shift needs g_engine. GPU mode: the CPU memmove trick is
    // moot (no g_blitPixels mirror) — a null hook makes ChromeClient::scroll
    // fall back to addDamage(clip), i.e. a clipped GPU repaint.
    BIB::g_scrollBlit = g_gpu ? nullptr : bibScrollBlit;

    // G3: guest 2D canvases on Ganesh (host defaults this ON with GPU after
    // the A/B — 600-arc anim 51.97/29.08/18.73 ms per frame cpu/gpu/this,
    // getImageData unregressed; ?canvasgpu=0 escapes).
    // CanvasUsesAcceleratedDrawing defaults FALSE for WebCore-direct
    // embedders, so without this guest canvases paint CPU-raster and
    // re-upload per draw even under GPU. Gated on the SURVIVING g_gpu
    // (after the surface fallback above): texture canvases drawn into a
    // raster window surface would read back on every draw.
    if (g_gpu && MAIN_THREAD_EM_ASM_INT({ return Module.bibCanvasGPU ? 1 : 0; }))
        g_engine->page->settings().setCanvasUsesAcceleratedDrawing(true);

    if (!paintFrame()) {
        printf("EMBEDDER: FAIL paint\n");
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
    }
    printf("EMBEDDER: paint OK\n");

    if (interactive) {
        // Browser mode: hand control to the host page's rAF loop. The unwind
        // skips the rest of main and keeps the runtime (and g_engine) alive.
        printf("EMBEDDER: interactive — runtime stays alive\n");
        MAIN_THREAD_ASYNC_EM_ASM({ if (Module.onEngineReady) Module.onEngineReady(); });
        emscripten_exit_with_live_runtime();
    }

    SkPixmap pixmap;
    if (!g_engine->surface->peekPixels(&pixmap)) {
        printf("EMBEDDER: FAIL peekPixels\n");
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
    }
    if (!writePPM("/out.ppm", pixmap)) {
        printf("EMBEDDER: FAIL writePPM\n");
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
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
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
    }
    if (redGlyph < 200) {
        printf("EMBEDDER: FAIL heading glyphs missing (fonts broken?)\n");
        exit(1); // EXIT_RUNTIME=0: explicit teardown (node gate path)
    }
    printf("EMBEDDER: DONE\n");
    exit(0); // EXIT_RUNTIME=0: explicit teardown runs Module.onExit (gate1 reads /out.ppm there)
}
