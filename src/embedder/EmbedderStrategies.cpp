// PlatformStrategies for the embedder. WebCore requires one to be installed
// before any load completes (FrameLoader::pageLoadCompleted and
// CachedResourceLoader::servePendingRequests dereference the loader strategy
// unconditionally).
//
// Phase 4: the loader strategy is REAL — every load is driven by
// WebCore::CurlRequest (the engine that WebKit2's NetworkDataTaskCurl wraps;
// there is no curl ResourceHandle in 2.52, its start() is
// ASSERT_NOT_REACHED on USE(CURL) ports). Shape:
//   loadResource -> SubresourceLoader::create -> BibResourceLoad(CurlRequest)
//   CurlRequestClient callbacks -> ResourceLoader::didReceiveResponse /
//   didReceiveBuffer / didFinishLoading / didFail — the same public feeding
//   interface WebKit2's WebResourceLoader uses.
// data: URLs go through loader->start(), which handles them before the
// (unreachable-on-curl) ResourceHandle path.
//
// Cookies: ONE in-memory NetworkStorageSession (CookieJarDB ":memory:")
// shared by the DOM CookieJar (installed on PageConfiguration in main.cpp)
// and the network path here (Cookie request header + Set-Cookie response
// storage — the NetworkDataTaskCurl recipe). Cookies survive in-engine
// navigations but not a host-page reload; OPFS persistence is later work.
//
// Not yet implemented (documented gaps, revisit with auth work):
// HTTP auth challenges (401 renders as the error body), sync XHR,
// ping loads, preconnect.

#include "config.h"

#include "BlobRegistry.h"
#include "BlobRegistryImpl.h"
#include "CachedResource.h"
#include "CookieJar.h"
#include "CookieJarDB.h"
#include "CurlRequest.h"
#include "CurlRequestClient.h"
#include "CurlResponse.h"
#include "HTTPHeaderNames.h"
#include "LoaderStrategy.h"
#include "MediaStrategy.h"
#include "NetworkLoadMetrics.h"
#include "NetworkStateNotifier.h"
#include "NetworkStorageSession.h"
#include "PasteboardStrategy.h"
#include "PlatformStrategies.h"
#include "ResourceError.h"
#include "ResourceLoader.h"
#include "ResourceLoaderIdentifier.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SameSiteInfo.h"
#include "ShouldRelaxThirdPartyCookieBlocking.h"
#include "StorageSessionProvider.h"
#include "SubresourceLoader.h"
#include "UserAgent.h"
#include "bib_host.h"
#include "bib_ua.h"
#include "FormData.h"
#include <emscripten.h>
#include <pal/SessionID.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>

namespace BIB {

using namespace WebCore;

static const String& embedderErrorDomain()
{
    static NeverDestroyed<String> domain { "BrowserInBrowserEmbedder"_s };
    return domain;
}

// The single cookie store for the whole embedder. Default-session semantics
// (matches the Page's PAL::SessionID::defaultSessionID()), but the database
// is forced to sqlite ":memory:" — the default-session path would be a MEMFS
// file that vanishes on host-page reload anyway, so skip the FS entirely.
NetworkStorageSession& embedderStorageSession()
{
    static NetworkStorageSession* session = [] {
        auto* session = new NetworkStorageSession(PAL::SessionID::defaultSessionID());
        session->setCookieDatabase(makeUniqueRef<CookieJarDB>(":memory:"_s));
        return session;
    }();
    return *session;
}

// DOM-side bridge: main.cpp hands this to CookieJar::create() on the
// PageConfiguration, replacing pageConfigurationWithEmptyClients'
// EmptyStorageSessionProvider (null session = document.cookie no-ops =
// Google's "Cookies are disabled" page).
class EmbedderStorageSessionProvider final : public StorageSessionProvider {
public:
    static Ref<EmbedderStorageSessionProvider> create() { return adoptRef(*new EmbedderStorageSessionProvider); }

private:
    NetworkStorageSession* storageSession() const final { return &embedderStorageSession(); }
};

Ref<WebCore::StorageSessionProvider> createEmbedderStorageSessionProvider()
{
    return EmbedderStorageSessionProvider::create();
}

// Cookie attach shared by document loads (BibResourceLoad) and pings —
// the NetworkDataTaskCurl::appendCookieHeader recipe.
static void appendEmbedderCookieHeader(ResourceRequest& request)
{
    auto includeSecureCookies = request.url().protocolIs("https"_s) ? IncludeSecureCookies::Yes : IncludeSecureCookies::No;
    auto cookieHeaderField = embedderStorageSession().cookieRequestHeaderFieldValue(request.firstPartyForCookies(), SameSiteInfo::create(request), request.url(), std::nullopt, std::nullopt, includeSecureCookies, ApplyTrackingPrevention::Yes, ShouldRelaxThirdPartyCookieBlocking::No, IsKnownCrossSiteTracker::No).first;
    if (!cookieHeaderField.isEmpty())
        request.addHTTPHeaderField(HTTPHeaderName::Cookie, cookieHeaderField);
}

// Fire-and-forget ping (navigator.sendBeacon, <a ping>, CSP reports): a
// minimal CurlRequestClient that owns itself for one request's lifetime.
// The BibResourceLoad machinery needs a ResourceLoader; pings have none by
// design. Headers are enough — we finish on response (no body read) and
// don't follow redirects (rare for beacon endpoints; documented gap).
class BibPingLoad final : public RefCounted<BibPingLoad>, public CurlRequestClient {
public:
    static void start(ResourceRequest&& request, LoaderStrategy::PingLoadCompletionHandler&& completionHandler)
    {
        Ref ping = adoptRef(*new BibPingLoad(WTF::move(completionHandler)));
        ping->m_selfRef = ping.copyRef(); // released in finish()
        bib_host_net_inflight(1);
        ping->m_counted = true;
        if (request.httpUserAgent().isEmpty())
            request.setHTTPUserAgent(bibUserAgent());
        appendEmbedderCookieHeader(request);
        ping->m_curlRequest = CurlRequest::create(request, ping.get());
        ping->m_curlRequest->resume();
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    explicit BibPingLoad(LoaderStrategy::PingLoadCompletionHandler&& completionHandler)
        : m_completionHandler(WTF::move(completionHandler))
    {
    }

    // Idempotent: the cancel() below invalidates the client, so no second
    // curl callback arrives after the first finish.
    void finish(const ResourceError& error)
    {
        if (std::exchange(m_counted, false))
            bib_host_net_inflight(-1);
        if (auto handler = std::exchange(m_completionHandler, nullptr))
            handler(error, m_response);
        if (auto curlRequest = std::exchange(m_curlRequest, nullptr)) {
            curlRequest->cancel();
            curlRequest->invalidateClient();
        }
        m_selfRef = nullptr;
    }

    void curlDidSendData(CurlRequest&, unsigned long long, unsigned long long) final { }

    void curlDidReceiveResponse(CurlRequest& request, CurlResponse&& curlResponse) final
    {
        m_response = ResourceResponse(curlResponse);
        // Beacon endpoints set cookies; store them like a real load would.
        static constexpr auto setCookieHeader = "set-cookie:"_s;
        for (const auto& header : curlResponse.headers) {
            if (header.startsWithIgnoringASCIICase(setCookieHeader)) {
                String setCookieString = header.right(header.length() - setCookieHeader.length()).trim(isASCIIWhitespace<char16_t>);
                embedderStorageSession().setCookiesFromHTTPResponse(request.resourceRequest().firstPartyForCookies(), curlResponse.url, setCookieString);
            }
        }
        finish({ });
    }

    void curlDidReceiveData(CurlRequest&, Ref<SharedBuffer>&&) final { }
    void curlDidComplete(CurlRequest&, NetworkLoadMetrics&&) final { finish({ }); }
    void curlDidFailWithError(CurlRequest&, ResourceError&& error, CertificateInfo&&) final { finish(error); }

    LoaderStrategy::PingLoadCompletionHandler m_completionHandler;
    RefPtr<CurlRequest> m_curlRequest;
    RefPtr<BibPingLoad> m_selfRef;
    ResourceResponse m_response;
    bool m_counted { false };
};

class BibResourceLoad;
// Loads waiting for a host transport answer, by request id (bib_fetch_done).
static HashMap<int, RefPtr<BibResourceLoad>>& hostFetchRegistry()
{
    static NeverDestroyed<HashMap<int, RefPtr<BibResourceLoad>>> map;
    return map.get();
}
// Asked per request: the host can switch transports between renders.
static bool hostFetchEnabled()
{
    return bib_host_flag("hostfetch") != 0;
}

// Drives ONE ResourceLoader through one (or, across redirects, several)
// CurlRequest. A stripped-down NetworkDataTaskCurl: no credential storage,
// no downloads, no auth restarts. Cookies use the NetworkDataTaskCurl
// recipe against embedderStorageSession().
class BibResourceLoad final : public RefCounted<BibResourceLoad>, public CurlRequestClient {
public:
    // cacheable: subresource GETs may be served from / stored into the host
    // resource cache (bib_host.h); main documents never are.
    static Ref<BibResourceLoad> create(ResourceLoader& loader, bool cacheable, Function<void(BibResourceLoad&)>&& doneCallback)
    {
        return adoptRef(*new BibResourceLoad(loader, cacheable, WTF::move(doneCallback)));
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    ResourceLoader* loader() const { return m_loader.get(); }

    void start()
    {
        bib_host_net_inflight(1);
        m_counted = true;
        startRequest(ResourceRequest { m_loader->request() });
    }

    // Every hop (initial load and each redirect) comes through here: a fresh
    // host-cache entry for the URL is delivered without touching the network.
    void startRequest(ResourceRequest&& request)
    {
        if (m_cacheable && request.httpMethod() == "GET"_s && request.url().protocolIsInHTTPFamily() && serveFromHostCache(request))
            return;
        if (hostFetchEnabled() && request.url().protocolIsInHTTPFamily() && startHostFetch(request))
            return;
        m_curlRequest = createCurlRequest(WTF::move(request));
        m_curlRequest->resume();
    }

    // Host transport: the same request curl would have sent (UA, cookies),
    // flattened body; the answer comes back through bib_fetch_done.
    bool startHostFetch(ResourceRequest& request)
    {
        if (request.httpUserAgent().isEmpty())
            request.setHTTPUserAgent(bibUserAgent());
        appendCookieHeader(request);
        Vector<uint8_t> body;
        if (auto formData = request.httpBody()) {
            if (formData->containsBlobElement())
                return false; // let curl stream it
            body = formData->flatten();
        }
        StringBuilder headers;
        for (const auto& field : request.httpHeaderFields()) {
            headers.append(field.key);
            headers.append(": "_s);
            headers.append(field.value);
            headers.append("\r\n"_s);
        }
        static int nextID = 0;
        m_hostFetchID = ++nextID;
        m_hostFetchRequest = request;
        CString url = request.url().string().utf8();
        CString method = request.httpMethod().utf8();
        CString hdr = headers.toString().utf8();
        hostFetchRegistry().set(m_hostFetchID, this);
        if (!bib_host_fetch(m_hostFetchID, method.data(), url.data(), hdr.data(), static_cast<int>(hdr.length()), body.span().data(), static_cast<int>(body.size()))) {
            hostFetchRegistry().remove(m_hostFetchID);
            m_hostFetchID = 0;
            return false;
        }
        return true;
    }

    // bib_fetch_done for this load: cookies, redirects and delivery follow the
    // curl path (curlDidReceiveResponse) with a synthetic CurlResponse.
    void hostFetchDone(int status, String&& headerBlock, Ref<SharedBuffer>&& body, int errnum)
    {
        m_hostFetchID = 0;
        if (!m_loader)
            return;
        if (errnum) {
            WTFLogAlways("BIB: host fetch failed errno=%d %s", errnum, m_hostFetchRequest.url().string().utf8().data());
            didFailInternal(ResourceError(errnum, m_hostFetchRequest.url()));
            return;
        }
        CurlResponse response = cachedResponseFor(m_hostFetchRequest.url(), status, headerBlock, body->size());
        m_response = ResourceResponse(response);
        storeResponseCookies(m_hostFetchRequest, response);
        if (serveRevalidated(response))
            return;
        m_staleBody = nullptr;
        if (shouldRedirect()) {
            performRedirect();
            return;
        }
        if (m_cacheable && status == 200 && m_hostFetchRequest.httpMethod() == "GET"_s && body->size() <= maxCollectBytes) {
            CString url = m_hostFetchRequest.url().string().utf8();
            CString hdr = headerBlock.utf8();
            bib_host_cache_put(url.data(), status, hdr.data(), static_cast<int>(hdr.length()), body->span().data(), static_cast<int>(body->size()));
        }
        deliverResponse(WTF::move(response), WTF::move(body));
    }

    // Parses a "Name: value\r\n" block into CurlResponse header lines, with a
    // synthetic status line first.
    static CurlResponse cachedResponseFor(const URL& url, int status, const String& block, long long length)
    {
        CurlResponse response;
        response.url = url;
        response.statusCode = status;
        response.expectedContentLength = length;
        response.httpVersion = CURL_HTTP_VERSION_1_1;
        response.headers.append(makeString("HTTP/1.1 "_s, status, " OK"_s));
        for (auto line : StringView(block).split('\n')) {
            auto trimmed = line.trim(isASCIIWhitespace<char16_t>);
            if (!trimmed.isEmpty())
                response.headers.append(trimmed.toString());
        }
        return response;
    }

    static String headerValueFromBlock(const String& block, ASCIILiteral name)
    {
        for (auto line : StringView(block).split('\n')) {
            auto colon = line.find(':');
            if (colon == notFound)
                continue;
            if (equalIgnoringASCIICase(line.left(colon).trim(isASCIIWhitespace<char16_t>), name))
                return line.substring(colon + 1).trim(isASCIIWhitespace<char16_t>).toString();
        }
        return String();
    }

    // A fresh hit is delivered from the cache. A stale hit turns the request
    // into a conditional one (If-None-Match / If-Modified-Since) and keeps
    // the entry for a 304 (curlDidReceiveResponse).
    bool serveFromHostCache(ResourceRequest& request)
    {
        CString url = request.url().string().utf8();
        int status = 0, headersLen = 0, bodyLen = 0, fresh = 0;
        char* headers = nullptr;
        uint8_t* body = nullptr;
        if (!bib_host_cache_get(url.data(), &status, &headers, &headersLen, &body, &bodyLen, &fresh))
            return false;
        String block = String::fromUTF8(std::span(headers, static_cast<size_t>(headersLen)));
        free(headers);
        Ref<SharedBuffer> cachedBody = SharedBuffer::create(std::span<const uint8_t>(body, static_cast<size_t>(bodyLen)));
        free(body);
        if (!fresh) {
            String etag = headerValueFromBlock(block, "ETag"_s);
            String lastModified = headerValueFromBlock(block, "Last-Modified"_s);
            if (etag.isEmpty() && lastModified.isEmpty())
                return false; // nothing to validate with: plain reload
            if (!etag.isEmpty())
                request.setHTTPHeaderField(HTTPHeaderName::IfNoneMatch, etag);
            if (!lastModified.isEmpty())
                request.setHTTPHeaderField(HTTPHeaderName::IfModifiedSince, lastModified);
            m_staleURL = request.url();
            m_staleBody = WTF::move(cachedBody);
            return false;
        }
        m_fromHostCache = true;
        deliverCached(cachedResponseFor(request.url(), status, block, bodyLen), WTF::move(cachedBody));
        return true;
    }

    void deliverCached(CurlResponse&& response, Ref<SharedBuffer>&& cachedBody)
    {
        deliverResponse(WTF::move(response), WTF::move(cachedBody), ResourceResponse::Source::DiskCache);
    }

    // Deliver like ResourceLoader::loadDataURL does: outside the caller's
    // stack, response then data then completion in one task.
    void deliverResponse(CurlResponse&& response, Ref<SharedBuffer>&& cachedBody, ResourceResponse::Source source = ResourceResponse::Source::Network)
    {
        callOnMainThread([this, protectedThis = Ref { *this }, cachedResponse = WTF::move(response), cachedBody = WTF::move(cachedBody), source]() mutable {
            if (!m_loader)
                return;
            m_response = ResourceResponse(cachedResponse);
            m_response.setSource(source);
            Ref<SharedBuffer> data = WTF::move(cachedBody);
            m_loader->didReceiveResponse(ResourceResponse { m_response }, [this, protectedThis = Ref { *this }, data]() mutable {
                if (!m_loader)
                    return;
                if (data->size())
                    m_loader->didReceiveBuffer(data.get(), data->size(), DataPayloadBytes);
                RefPtr loader = m_loader;
                notifyDone();
                loader->didFinishLoading(NetworkLoadMetrics { });
            });
        });
    }

    // 304 for a conditional request on a stale host-cache entry: the host
    // merges the 304's headers into the entry and returns the block to deliver
    // with the cached body; the curl transfer (no body) is dropped.
    bool serveRevalidated(CurlRequest&, const CurlResponse& curlResponse) { return serveRevalidated(curlResponse); }

    bool serveRevalidated(const CurlResponse& curlResponse)
    {
        if (!m_staleBody || curlResponse.statusCode != 304 || curlResponse.url != m_staleURL)
            return false;
        StringBuilder hdr;
        for (const auto& header : curlResponse.headers) {
            hdr.append(header);
            hdr.append("\r\n"_s);
        }
        CString block304 = hdr.toString().utf8();
        CString url = m_staleURL.string().utf8();
        char* merged = nullptr;
        int mergedLen = 0;
        if (!bib_host_cache_touch(url.data(), block304.data(), static_cast<int>(block304.length()), &merged, &mergedLen))
            return false;
        String block = String::fromUTF8(std::span(merged, static_cast<size_t>(mergedLen)));
        free(merged);
        Ref<SharedBuffer> body = m_staleBody.releaseNonNull();
        if (auto curlRequest = std::exchange(m_curlRequest, nullptr)) {
            curlRequest->cancel();
            curlRequest->invalidateClient();
        }
        m_fromHostCache = true;
        deliverCached(cachedResponseFor(m_staleURL, 200, block, body->size()), WTF::move(body));
        return true;
    }

    // Idempotent; also reached re-entrantly via LoaderStrategy::remove()
    // when didFinishLoading/didFail release the loader's resources.
    void cancel()
    {
        m_loader = nullptr;
        if (auto curlRequest = std::exchange(m_curlRequest, nullptr)) {
            curlRequest->cancel();
            curlRequest->invalidateClient();
        }
        if (int id = std::exchange(m_hostFetchID, 0)) {
            hostFetchRegistry().remove(id);
            bib_host_fetch_cancel(id);
        }
        if (std::exchange(m_counted, false))
            bib_host_net_inflight(-1);
    }

private:
    BibResourceLoad(ResourceLoader& loader, bool cacheable, Function<void(BibResourceLoad&)>&& doneCallback)
        : m_loader(&loader)
        , m_doneCallback(WTF::move(doneCallback))
        , m_cacheable(cacheable)
    {
    }

    // Host cache feed: a complete 200 GET response is offered to the host at
    // completion (the host applies the Cache-Control rules and size caps).
    static constexpr size_t maxCollectBytes = 8 * 1024 * 1024;

    void beginCollect(const CurlRequest& request, const CurlResponse& curlResponse)
    {
        m_collect = false;
        m_collected.clear();
        if (!m_cacheable || curlResponse.statusCode != 200 || request.resourceRequest().httpMethod() != "GET"_s)
            return;
        StringBuilder headers;
        for (const auto& header : curlResponse.headers) {
            headers.append(header);
            headers.append("\r\n"_s);
        }
        m_collectURL = request.resourceRequest().url().string().utf8();
        m_collectHeaders = headers.toString().utf8();
        m_collect = true;
    }

    void collect(const SharedBuffer& buffer)
    {
        if (!m_collect)
            return;
        if (m_collected.size() + buffer.size() > maxCollectBytes) {
            m_collect = false;
            m_collected.clear();
            return;
        }
        m_collected.append(buffer.span());
    }

    void finishCollect()
    {
        if (!m_collect)
            return;
        m_collect = false;
        bib_host_cache_put(m_collectURL.data(), 200, m_collectHeaders.data(), static_cast<int>(m_collectHeaders.length()), m_collected.span().data(), static_cast<int>(m_collected.size()));
        m_collected.clear();
    }

    Ref<CurlRequest> createCurlRequest(ResourceRequest&& request)
    {
        // No FrameLoaderClient supplies a UA on this embedder; sites
        // misbehave badly with an empty one.
        if (request.httpUserAgent().isEmpty())
            request.setHTTPUserAgent(bibUserAgent());
        appendCookieHeader(request);
        return CurlRequest::create(request, *this);
    }

    // NetworkDataTaskCurl::appendCookieHeader. Applied to the COPY handed to
    // CurlRequest (initial load and every redirect hop both come through
    // createCurlRequest); the ResourceLoader's own request never carries the
    // header, so hops can't accumulate stale Cookie values.
    void appendCookieHeader(ResourceRequest& request)
    {
        appendEmbedderCookieHeader(request);
    }

    // NetworkDataTaskCurl::handleCookieHeaders. Runs on EVERY response,
    // including 3xx — login/consent flows set cookies on the redirect leg.
    // Prefix match is "set-cookie:" WITHOUT the space (upstream requires
    // "set-cookie: " and silently drops a legal space-less header; the
    // value is trimmed instead — Codex 2026-06-10).
    void storeResponseCookies(const ResourceRequest& request, const CurlResponse& response)
    {
        static constexpr auto setCookieHeader = "set-cookie:"_s;
        for (const auto& header : response.headers) {
            if (header.startsWithIgnoringASCIICase(setCookieHeader)) {
                String setCookieString = header.right(header.length() - setCookieHeader.length()).trim(isASCIIWhitespace<char16_t>);
                embedderStorageSession().setCookiesFromHTTPResponse(request.firstPartyForCookies(), response.url, setCookieString);
            }
        }
    }

    void notifyDone()
    {
        if (std::exchange(m_counted, false))
            bib_host_net_inflight(-1);
        if (auto doneCallback = std::exchange(m_doneCallback, nullptr))
            doneCallback(*this);
    }

    bool shouldRedirect() const
    {
        auto statusCode = m_response.httpStatusCode();
        if (statusCode < 300 || statusCode >= 400)
            return false;
        // Some 3xx status codes aren't actually redirects.
        if (statusCode == 300 || statusCode == 304 || statusCode == 305 || statusCode == 306)
            return false;
        return !m_response.httpHeaderField(HTTPHeaderName::Location).isEmpty();
    }

    bool shouldRedirectAsGET(const ResourceRequest& request, bool crossOrigin) const
    {
        if (request.httpMethod() == "GET"_s || request.httpMethod() == "HEAD"_s)
            return false;
        if (!request.url().protocolIsInHTTPFamily())
            return true;
        if (m_response.isSeeOther())
            return true;
        if ((m_response.isMovedPermanently() || m_response.isFound()) && (request.httpMethod() == "POST"_s))
            return true;
        if (crossOrigin && (request.httpMethod() == "DELETE"_s))
            return true;
        return false;
    }

    void performRedirect()
    {
        static const int maxRedirects = 20;
        if (m_redirectCount++ > maxRedirects) {
            didFailInternal(ResourceError(CURLE_TOO_MANY_REDIRECTS, m_response.url()));
            return;
        }

        URL redirectedURL { m_response.url(), m_response.httpHeaderField(HTTPHeaderName::Location) };
        // A remote response must never redirect into the local (MEMFS)
        // filesystem — CurlRequest has a real file: path (Codex 2026-06-10,
        // matches NetworkDataTaskCurl::willPerformHTTPRedirection).
        if (redirectedURL.protocolIsFile()) {
            didFailInternal(ResourceError(CURLE_FILE_COULDNT_READ_FILE, m_response.url()));
            return;
        }
        ResourceRequest request = m_loader->request();
        if (!redirectedURL.hasFragmentIdentifier() && request.url().hasFragmentIdentifier())
            redirectedURL.setFragmentIdentifier(request.url().fragmentIdentifier());

        bool isCrossOrigin = !protocolHostAndPortAreEqual(request.url(), redirectedURL);
        request.setURL(WTF::move(redirectedURL));

        if (!equalLettersIgnoringASCIICase(request.httpMethod(), "get"_s)) {
            if (!request.url().protocolIsInHTTPFamily() || shouldRedirectAsGET(request, isCrossOrigin)) {
                request.setHTTPMethod("GET"_s);
                request.setHTTPBody(nullptr);
                request.clearHTTPContentType();
            }
        }

        request.removeCredentials();
        if (isCrossOrigin) {
            request.clearHTTPAuthorization();
            request.clearHTTPOrigin();
        }

        ResourceResponse redirectResponse { m_response };
        // First-party-for-cookies on main-frame redirects is updated INSIDE
        // this call: SubresourceLoader -> CachedRawResource::redirectReceived
        // -> DocumentLoader::willSendRequest does
        // setFirstPartyForCookies(newURL) for the main frame (subframes
        // keep the main document's first party, per spec). The cookie
        // attach in createCurlRequest below therefore sees the updated
        // value (Codex 2026-06-10 — verified, no fork from upstream).
        m_loader->willSendRequest(WTF::move(request), redirectResponse, [this, protectedThis = Ref { *this }](ResourceRequest&& newRequest) {
            if (newRequest.isNull() || !m_loader) {
                // Policy/CSP cancelled the redirect. The loader tears itself
                // down separately — but the current CurlRequest is parked
                // waiting for completeDidReceiveResponse and must not leak
                // (Codex 2026-06-10).
                if (auto curlRequest = std::exchange(m_curlRequest, nullptr)) {
                    curlRequest->cancel();
                    curlRequest->invalidateClient();
                }
                notifyDone();
                return;
            }
            if (auto curlRequest = std::exchange(m_curlRequest, nullptr)) {
                curlRequest->cancel();
                curlRequest->invalidateClient();
            }
            startRequest(WTF::move(newRequest));
        });
    }

    void didFailInternal(const ResourceError& error)
    {
        RefPtr loader = m_loader;
        notifyDone(); // removes the registry ref; didFail re-enters remove() harmlessly
        if (loader)
            loader->didFail(error);
    }

    // CurlRequestClient — all callbacks arrive on the main thread via
    // callOnMainThread (never from inside curl_multi_perform; see the
    // CurlRequest::runOnMainThread Emscripten patch).
    void curlDidSendData(CurlRequest&, unsigned long long bytesSent, unsigned long long totalBytesToBeSent) final
    {
        Ref protectedThis { *this };
        if (m_loader)
            m_loader->didSendData(bytesSent, totalBytesToBeSent);
    }

    void curlDidReceiveResponse(CurlRequest& request, CurlResponse&& curlResponse) final
    {
        Ref protectedThis { *this };
        if (!m_loader || m_curlRequest != &request)
            return;

        m_response = ResourceResponse(curlResponse);

        storeResponseCookies(request.resourceRequest(), curlResponse);

        if (serveRevalidated(request, curlResponse))
            return;
        m_staleBody = nullptr;

        if (shouldRedirect()) {
            performRedirect();
            return;
        }

        beginCollect(request, curlResponse);
        m_loader->didReceiveResponse(ResourceResponse { m_response }, [this, protectedThis = Ref { *this }] {
            if (m_curlRequest && m_loader)
                m_curlRequest->completeDidReceiveResponse();
        });
    }

    void curlDidReceiveData(CurlRequest& request, Ref<SharedBuffer>&& buffer) final
    {
        Ref protectedThis { *this };
        if (!m_loader || m_curlRequest != &request)
            return;
        collect(buffer.get());
        long long size = buffer->size();
        m_loader->didReceiveBuffer(buffer.get(), size, DataPayloadBytes);
    }

    void curlDidComplete(CurlRequest& request, NetworkLoadMetrics&& metrics) final
    {
        Ref protectedThis { *this };
        if (!m_loader || m_curlRequest != &request)
            return;
        finishCollect();
        RefPtr loader = m_loader;
        notifyDone();
        loader->didFinishLoading(metrics);
    }

    void curlDidFailWithError(CurlRequest& request, ResourceError&& error, CertificateInfo&&) final
    {
        Ref protectedThis { *this };
        // Failures are easy to lose in a canvas-only embedder — always log.
        WTFLogAlways("BIB: load failed curl=%d %s (%s)", error.errorCode(), error.failingURL().string().utf8().data(), error.localizedDescription().utf8().data());
        if (!m_loader || m_curlRequest != &request)
            return;
        didFailInternal(error);
    }

    RefPtr<ResourceLoader> m_loader;
    RefPtr<CurlRequest> m_curlRequest;
    Function<void(BibResourceLoad&)> m_doneCallback;
    ResourceResponse m_response;
    int m_redirectCount { 0 };
    bool m_cacheable { false };
    bool m_counted { false };
    bool m_fromHostCache { false };
    URL m_staleURL;
    RefPtr<SharedBuffer> m_staleBody; // stale host-cache entry awaiting a 304
    int m_hostFetchID { 0 };
    ResourceRequest m_hostFetchRequest;
    bool m_collect { false };
    CString m_collectURL;
    CString m_collectHeaders;
    Vector<uint8_t> m_collected;
};

// Host transport completion (bib_host.h). Runs on the engine thread; the
// buffers are bib_wasm_alloc'd by the host and freed here.
extern "C" EMSCRIPTEN_KEEPALIVE void bib_fetch_done(int id, int status, char* headers, int headersLen, uint8_t* body, int bodyLen, int errnum)
{
    String headerBlock = headers ? String::fromUTF8(std::span(headers, static_cast<size_t>(headersLen))) : String();
    Ref<SharedBuffer> data = body ? SharedBuffer::create(std::span<const uint8_t>(body, static_cast<size_t>(bodyLen))) : SharedBuffer::create();
    free(headers);
    free(body);
    if (auto load = hostFetchRegistry().take(id))
        load->hostFetchDone(status, WTF::move(headerBlock), WTF::move(data), errnum);
}

// --- Request blocklist ------------------------------------------------------
// CLoop parses every byte of script on the main thread, so analytics/ads/
// telemetry bundles (GTM, adsbygoogle, sentry, segment, ...) are pure boot
// cost — and several also fail over wisp with curl=35 noise. Refusing them in
// loadResource() means they never download OR parse. Main resources are
// exempt (navigations and iframe documents still load), and consent managers
// (OneTrust/cookielaw) are deliberately NOT listed — sites gate functionality
// on their callbacks. ?noblock=1 on the host page disables the list.
static bool g_requestBlocklistEnabled = true;

void setRequestBlocklistEnabled(bool enabled)
{
    g_requestBlocklistEnabled = enabled;
}

static bool isBlocklistedHost(StringView host)
{
    static constexpr ASCIILiteral blockedSuffixes[] = {
        // Google ads + analytics. GTM is a known tradeoff: rare sites wire
        // functional logic through it, but gtm.js + the tags it injects are
        // the single biggest parse cost on marketing pages — ?noblock=1 is
        // the escape hatch.
        "google-analytics.com"_s,
        "googletagmanager.com"_s,
        "googletagservices.com"_s,
        "googlesyndication.com"_s,
        "googleadservices.com"_s,
        "adservice.google.com"_s,
        "doubleclick.net"_s,
        // Error/telemetry reporters
        "sentry.io"_s,
        "sentry-cdn.com"_s,
        "bugsnag.com"_s,
        "nr-data.net"_s,
        "newrelic.com"_s,
        "datadoghq-browser-agent.com"_s,
        "browser-intake-datadoghq.com"_s,
        "cloudflareinsights.com"_s,
        // Product analytics
        "segment.com"_s,
        "segment.io"_s,
        "mixpanel.com"_s,
        "amplitude.com"_s,
        "hotjar.com"_s,
        "fullstory.com"_s,
        "clarity.ms"_s,
        "quantserve.com"_s,
        "scorecardresearch.com"_s,
        "chartbeat.com"_s,
        "mc.yandex.ru"_s,
        // Social pixels / ad networks. facebook.net is deliberately absent:
        // connect.facebook.net also serves the FB Login SDK (FB.login()),
        // so blocking it breaks "Login with Facebook" (Codex).
        "analytics.tiktok.com"_s,
        "static.ads-twitter.com"_s,
        "analytics.twitter.com"_s,
        "bat.bing.com"_s,
        "snap.licdn.com"_s,
        "px.ads.linkedin.com"_s,
        "amazon-adsystem.com"_s,
        "criteo.com"_s,
        "criteo.net"_s,
        "taboola.com"_s,
        "outbrain.com"_s,
    };
    // URL hosts are canonicalized to lowercase, so exact suffix matching is
    // enough; the dot check stops "notsentry.io" from matching "sentry.io".
    for (auto literal : blockedSuffixes) {
        StringView blocked { literal };
        if (host.length() < blocked.length() || !host.endsWith(blocked))
            continue;
        if (host.length() == blocked.length() || host[host.length() - blocked.length() - 1] == '.')
            return true;
    }
    return false;
}

class EmbedderLoaderStrategy final : public LoaderStrategy {
public:
    void scheduleLoad(ResourceLoader& loader, bool cacheable = false)
    {
        // ResourceLoader::start() handles data: URLs internally, before the
        // ResourceHandle path (which is unreachable on curl ports). blob:
        // URLs also work through start(): BlobRegistryImpl registers a
        // BlobResourceHandle constructor for the "blob" protocol in
        // ResourceHandle's builtin map, served from EmbedderBlobRegistry's
        // in-process impl — never curl.
        if (loader.request().url().protocolIsData() || loader.request().url().protocolIsBlob()) {
            loader.start();
            return;
        }

        auto load = BibResourceLoad::create(loader, cacheable, [this](BibResourceLoad& load) {
            if (auto* loader = load.loader())
                m_loads.remove(loader);
        });
        m_loads.set(&loader, load.copyRef());
        load->start();
    }

private:
    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request, const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final
    {
        // Null loader -> CachedResource::failBeforeStarting(): the documented
        // creation-failure path, so blocked scripts get ordinary error events.
        if (g_requestBlocklistEnabled && resource.type() != CachedResource::Type::MainResource && isBlocklistedHost(request.url().host())) {
            WTFLogAlways("BIB: blocked %s (request blocklist; ?noblock=1 to disable)", request.url().string().utf8().data());
            completionHandler(nullptr);
            return;
        }

        bool cacheable = resource.type() != CachedResource::Type::MainResource;
        SubresourceLoader::create(frame, resource, WTF::move(request), options, [this, cacheable, completionHandler = WTF::move(completionHandler)](RefPtr<SubresourceLoader>&& loader) mutable {
            if (loader)
                scheduleLoad(*loader, cacheable);
            completionHandler(WTF::move(loader));
        });
    }

    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest& request, ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error, ResourceResponse&, Vector<uint8_t>&) final
    {
        // Single-threaded build cannot block on the network. Sync XHR fails.
        error = ResourceError(embedderErrorDomain(), 0, request.url(), "Synchronous loads are not supported in this embedder"_s);
    }

    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }

    void remove(ResourceLoader* loader) final
    {
        if (auto load = m_loads.take(loader))
            load->cancel();
    }

    void setDefersLoading(ResourceLoader&, bool) final
    {
        // CurlRequest has no pause-after-start; defers is best-effort here.
    }

    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { }

    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { }
    void resumePendingRequests() final { }

    void startPingLoad(LocalFrame&, ResourceRequest& request, const HTTPHeaderMap&, const FetchOptions&, ContentSecurityPolicyImposition, PingLoadCompletionHandler&& completionHandler) final
    {
        // sendBeacon / <a ping> / CSP reports. Fire-and-forget through a
        // self-owning curl client — erroring these out made beacon-gated
        // code paths fail and spammed the console on every analytics-bearing
        // site. originalRequestHeaders/CORS are not applied (documented gap).
        if (g_requestBlocklistEnabled && isBlocklistedHost(request.url().host())) {
            // Complete as success: beacon callers cannot observe delivery,
            // and an error here would only trip SDK retry loops.
            WTFLogAlways("BIB: blocked beacon %s (request blocklist)", request.url().string().utf8().data());
            if (completionHandler)
                completionHandler({ }, { });
            return;
        }
        BibPingLoad::start(ResourceRequest { request }, WTF::move(completionHandler));
    }

    void preconnectTo(FrameLoader&, ResourceRequest&&, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completionHandler) final
    {
        // Succeed as a no-op: curl manages its own connection pool and a
        // warm-up dial isn't worth the stream churn over wisp. Reporting an
        // ERROR here (the old behavior) just generated console noise for
        // every <link rel=preconnect>.
        if (completionHandler)
            completionHandler({ });
    }

    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }

    bool isOnLine() const final { return true; }
    void addOnlineStateChangeListener(Function<void(bool)>&& listener) final
    {
        NetworkStateNotifier::singleton().addListener(WTF::move(listener));
    }

    void isResourceLoadFinished(CachedResource& resource, CompletionHandler<void(bool)>&& callback) final
    {
        if (!resource.loader()) {
            callback(true);
            return;
        }
        callback(!m_loads.contains(resource.loader()));
    }

    ResourceError cancelledError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "Load cancelled"_s, ResourceError::Type::Cancellation);
    }
    ResourceError blockedError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "Load blocked"_s, ResourceError::Type::AccessControl);
    }
    ResourceError blockedByContentBlockerError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "Blocked by content blocker"_s, ResourceError::Type::AccessControl);
    }
    ResourceError cannotShowURLError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "Cannot show URL"_s, ResourceError::Type::AccessControl);
    }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "Interrupted for policy change"_s);
    }
    ResourceError cannotShowMIMETypeError(const ResourceResponse& response) const final
    {
        return ResourceError(embedderErrorDomain(), 0, response.url(), "Cannot show MIME type"_s);
    }
    ResourceError fileDoesNotExistError(const ResourceResponse& response) const final
    {
        return ResourceError(embedderErrorDomain(), 0, response.url(), "File does not exist"_s);
    }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "HTTPS upgrade redirect loop"_s);
    }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const final
    {
        return ResourceError(embedderErrorDomain(), 0, request.url(), "HTTP navigation with HTTPS-only"_s);
    }
    ResourceError pluginWillHandleLoadError(const ResourceResponse& response) const final
    {
        return ResourceError(embedderErrorDomain(), 0, response.url(), "Plugin will handle load"_s);
    }

    HashMap<ResourceLoader*, Ref<BibResourceLoad>> m_loads;
};

class EmbedderMediaStrategy final : public MediaStrategy {
    // VIDEO / WEB_AUDIO / MEDIA_SOURCE are OFF: no pure virtuals remain.
};

// In-process BlobRegistry forwarding to WebCore's own BlobRegistryImpl —
// the single-process equivalent of WebKitLegacy's WebBlobRegistry. The
// blobRegistryImpl() override is what lets loaders resolve blob: data
// without IPC.
class EmbedderBlobRegistry final : public WebCore::BlobRegistry {
    void registerInternalFileBlobURL(const URL& url, Ref<BlobDataFileReference>&& file, const String&, const String& contentType) final
    {
        m_impl.registerInternalFileBlobURL(url, WTF::move(file), contentType);
    }

    void registerInternalBlobURL(const URL& url, Vector<BlobPart>&& parts, const String& contentType) final
    {
        m_impl.registerInternalBlobURL(url, WTF::move(parts), contentType);
    }

    void registerBlobURL(const URL& url, const URL& srcURL, const PolicyContainer& policyContainer, const std::optional<SecurityOriginData>& topOrigin) final
    {
        m_impl.registerBlobURL(url, srcURL, policyContainer, topOrigin);
    }

    void registerInternalBlobURLOptionallyFileBacked(const URL& url, const URL& srcURL, RefPtr<BlobDataFileReference>&& file, const String& contentType) final
    {
        m_impl.registerInternalBlobURLOptionallyFileBacked(url, srcURL, WTF::move(file), contentType, { });
    }

    void registerInternalBlobURLForSlice(const URL& url, const URL& srcURL, long long start, long long end, const String& contentType) final
    {
        m_impl.registerInternalBlobURLForSlice(url, srcURL, start, end, contentType);
    }

    void unregisterBlobURL(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final
    {
        m_impl.unregisterBlobURL(url, topOrigin);
    }

    void registerBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final
    {
        m_impl.registerBlobURLHandle(url, topOrigin);
    }

    void unregisterBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final
    {
        m_impl.unregisterBlobURLHandle(url, topOrigin);
    }

    String blobType(const URL& url) final { return m_impl.blobType(url); }
    unsigned long long blobSize(const URL& url) final { return m_impl.blobSize(url); }

    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& blobURLs, CompletionHandler<void(Vector<String>&& filePaths)>&& completionHandler) final
    {
        m_impl.writeBlobsToTemporaryFilesForIndexedDB(blobURLs, WTF::move(completionHandler));
    }

    BlobRegistryImpl* blobRegistryImpl() final { return &m_impl; }

    BlobRegistryImpl m_impl;
};

class EmbedderPlatformStrategies final : public PlatformStrategies {
    LoaderStrategy* createLoaderStrategy() final
    {
        static NeverDestroyed<EmbedderLoaderStrategy> strategy;
        return &strategy.get();
    }

    PasteboardStrategy* createPasteboardStrategy() final
    {
        // PasteboardEmscripten.cpp never consults the strategy.
        return nullptr;
    }

    MediaStrategy* createMediaStrategy() final
    {
        static NeverDestroyed<EmbedderMediaStrategy> strategy;
        return &strategy.get();
    }

    BlobRegistry* createBlobRegistry() final
    {
        // In-process registry backed by WebCore's BlobRegistryImpl (the
        // WebKitLegacy WebBlobRegistry pattern). Returning nullptr here
        // aborted the engine on the FIRST `new Blob(...)` any page ran —
        // blobRegistry() CheckedRefs the pointer (root cause #9;
        // google/ebay/Turnstile all died on it).
        static NeverDestroyed<EmbedderBlobRegistry> registry;
        return &registry.get();
    }
};

void installEmbedderStrategies()
{
    static NeverDestroyed<EmbedderPlatformStrategies> strategies;
    setPlatformStrategies(&strategies.get());
}

} // namespace BIB
