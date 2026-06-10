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
// Not yet implemented (documented gaps, revisit with cookies/auth work):
// cookies, HTTP auth challenges (401 renders as the error body), sync XHR,
// ping loads, preconnect.

#include "config.h"

#include "BlobRegistry.h"
#include "BlobRegistryImpl.h"
#include "CurlRequest.h"
#include "CurlRequestClient.h"
#include "CurlResponse.h"
#include "HTTPHeaderNames.h"
#include "LoaderStrategy.h"
#include "MediaStrategy.h"
#include "NetworkLoadMetrics.h"
#include "NetworkStateNotifier.h"
#include "PasteboardStrategy.h"
#include "PlatformStrategies.h"
#include "ResourceError.h"
#include "ResourceLoader.h"
#include "ResourceLoaderIdentifier.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SubresourceLoader.h"
#include "UserAgent.h"
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>

namespace BIB {

using namespace WebCore;

static const String& embedderErrorDomain()
{
    static NeverDestroyed<String> domain { "BrowserInBrowserEmbedder"_s };
    return domain;
}

// Drives ONE ResourceLoader through one (or, across redirects, several)
// CurlRequest. A stripped-down NetworkDataTaskCurl: no credential storage,
// no cookie jar, no downloads, no auth restarts.
class BibResourceLoad final : public RefCounted<BibResourceLoad>, public CurlRequestClient {
public:
    static Ref<BibResourceLoad> create(ResourceLoader& loader, Function<void(BibResourceLoad&)>&& doneCallback)
    {
        return adoptRef(*new BibResourceLoad(loader, WTF::move(doneCallback)));
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    ResourceLoader* loader() const { return m_loader.get(); }

    void start()
    {
        m_curlRequest = createCurlRequest(ResourceRequest { m_loader->request() });
        m_curlRequest->resume();
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
    }

private:
    BibResourceLoad(ResourceLoader& loader, Function<void(BibResourceLoad&)>&& doneCallback)
        : m_loader(&loader)
        , m_doneCallback(WTF::move(doneCallback))
    {
    }

    Ref<CurlRequest> createCurlRequest(ResourceRequest&& request)
    {
        // No FrameLoaderClient supplies a UA on this embedder; sites
        // misbehave badly with an empty one.
        if (request.httpUserAgent().isEmpty())
            request.setHTTPUserAgent(standardUserAgent());
        return CurlRequest::create(request, *this);
    }

    void notifyDone()
    {
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
            m_curlRequest = createCurlRequest(WTF::move(newRequest));
            m_curlRequest->resume();
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

        if (shouldRedirect()) {
            performRedirect();
            return;
        }

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
        long long size = buffer->size();
        m_loader->didReceiveBuffer(buffer.get(), size, DataPayloadBytes);
    }

    void curlDidComplete(CurlRequest& request, NetworkLoadMetrics&& metrics) final
    {
        Ref protectedThis { *this };
        if (!m_loader || m_curlRequest != &request)
            return;
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
};

class EmbedderLoaderStrategy final : public LoaderStrategy {
public:
    void scheduleLoad(ResourceLoader& loader)
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

        auto load = BibResourceLoad::create(loader, [this](BibResourceLoad& load) {
            if (auto* loader = load.loader())
                m_loads.remove(loader);
        });
        m_loads.set(&loader, load.copyRef());
        load->start();
    }

private:
    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request, const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final
    {
        SubresourceLoader::create(frame, resource, WTF::move(request), options, [this, completionHandler = WTF::move(completionHandler)](RefPtr<SubresourceLoader>&& loader) mutable {
            if (loader)
                scheduleLoad(*loader);
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
        if (completionHandler)
            completionHandler(ResourceError(embedderErrorDomain(), 0, request.url(), "Ping loads are not supported yet"_s), { });
    }

    void preconnectTo(FrameLoader&, ResourceRequest&& request, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completionHandler) final
    {
        if (completionHandler)
            completionHandler(ResourceError(embedderErrorDomain(), 0, request.url(), "Preconnect is not supported yet"_s));
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
