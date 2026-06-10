// PlatformStrategies for the Phase 2 embedder. WebCore requires one to be
// installed before any load completes (FrameLoader::pageLoadCompleted and
// CachedResourceLoader::servePendingRequests dereference the loader strategy
// unconditionally). Everything here is offline: subresource loads fail
// cleanly, the network is permanently "offline". The Wisp/curl-backed
// strategy replaces the loader half in Phase 4.
//
// Shape cribbed from WebKit/NetworkProcess/NetworkProcessPlatformStrategies
// (the most minimal in-tree implementation), with a real LoaderStrategy
// because ours actually gets called.

#include "config.h"

#include "LoaderStrategy.h"
#include "MediaStrategy.h"
#include "PasteboardStrategy.h"
#include "PlatformStrategies.h"
#include "ResourceError.h"
#include "ResourceLoaderIdentifier.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SubresourceLoader.h"
#include <wtf/NeverDestroyed.h>

namespace BIB {

using namespace WebCore;

static const String& embedderErrorDomain()
{
    static NeverDestroyed<String> domain { "BrowserInBrowserEmbedder"_s };
    return domain;
}

class EmbedderLoaderStrategy final : public LoaderStrategy {
    void loadResource(LocalFrame&, CachedResource&, ResourceRequest&&, const ResourceLoaderOptions&, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final
    {
        completionHandler(nullptr);
    }

    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest& request, ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error, ResourceResponse&, Vector<uint8_t>&) final
    {
        error = ResourceError(embedderErrorDomain(), 0, request.url(), "Offline embedder: no synchronous loads"_s);
    }

    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }

    void remove(ResourceLoader*) final { }
    void setDefersLoading(ResourceLoader&, bool) final { }
    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { }

    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { }
    void resumePendingRequests() final { }

    void startPingLoad(LocalFrame&, ResourceRequest& request, const HTTPHeaderMap&, const FetchOptions&, ContentSecurityPolicyImposition, PingLoadCompletionHandler&& completionHandler) final
    {
        if (completionHandler)
            completionHandler(ResourceError(embedderErrorDomain(), 0, request.url(), "Offline embedder: no ping loads"_s), { });
    }

    void preconnectTo(FrameLoader&, ResourceRequest&& request, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completionHandler) final
    {
        if (completionHandler)
            completionHandler(ResourceError(embedderErrorDomain(), 0, request.url(), "Offline embedder: no preconnect"_s));
    }

    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }

    bool isOnLine() const final { return false; }
    void addOnlineStateChangeListener(Function<void(bool)>&&) final { }

    void isResourceLoadFinished(CachedResource&, CompletionHandler<void(bool)>&& callback) final
    {
        callback(true);
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
};

class EmbedderMediaStrategy final : public MediaStrategy {
    // VIDEO / WEB_AUDIO / MEDIA_SOURCE are OFF: no pure virtuals remain.
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
        // No blob URLs in the offline gate; revisit with networking (Phase 4).
        return nullptr;
    }
};

void installEmbedderStrategies()
{
    static NeverDestroyed<EmbedderPlatformStrategies> strategies;
    setPlatformStrategies(&strategies.get());
}

} // namespace BIB
