// Guest WebSocket provider, phase WS-0: fail fast instead of killing the
// engine (task #58).
//
// pageConfigurationWithEmptyClients installs EmptySocketProvider, whose
// createWebSocketChannel returns nullptr — and WebSocket::create RELEASE_
// ASSERTs on that ("Every ScriptExecutionContext should have a
// SocketProvider"), so ANY guest `new WebSocket()` aborts the whole engine
// (discord.com/login dies on its remote-auth gateway socket). WebKit ≥2.46
// has no in-WebCore channel implementation left to borrow (it moved to the
// WebKit2 network process); SocketProvider::createWebSocketChannel is pure
// virtual, so the channel is ours to write — same situation as IndexedDB
// (BibIDBServer).
//
// WS-0 semantics: connect() reports failure through the client exactly like
// a real unreachable server — didReceiveMessageError (error event) then
// didClose with wasClean=false / code 1006 (close event). Both client
// methods self-queue via queueTaskKeepingObjectAlive inside WebSocket, so
// calling them synchronously from connect() (mid-constructor) is safe: the
// events dispatch only after author script can attach handlers. Sites see a
// failed connection and run their reconnect/offline paths; the engine
// lives. WS-1 (real channel over curl 8.17's native WebSocket API on the
// existing curl+OpenSSL+wisp stack) replaces this class.

#pragma once

#include "Document.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SocketProvider.h"
#include "ThreadableWebSocketChannel.h"
#include "WebSocketChannelClient.h"
#include "WebTransportSession.h"
#include <JavaScriptCore/ConsoleTypes.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/MakeString.h>

namespace BIB {

class BibFailFastWebSocketChannel final : public RefCounted<BibFailFastWebSocketChannel>, public WebCore::ThreadableWebSocketChannel {
public:
    static Ref<BibFailFastWebSocketChannel> create(WebCore::Document& document, WebCore::WebSocketChannelClient& client)
    {
        return adoptRef(*new BibFailFastWebSocketChannel(document, client));
    }

    // AbstractRefCounted (via ThreadableWebSocketChannel) — same idiom as
    // WorkerThreadableWebSocketChannel.
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    ConnectStatus connect(const URL& url, const String&) final
    {
        if (RefPtr document = m_document.get()) {
            document->addConsoleMessage(JSC::MessageSource::Network, JSC::MessageLevel::Warning,
                makeString("BrowserInBrowser: guest WebSocket support is not implemented yet; failing connection to "_s, url.string()));
        }
        // Mirrors a refused/unreachable connection: error event, then a
        // not-wasClean close. didClose(1006) also makes WebSocket drop its
        // ref to this channel, so a later author close()/double-fail is
        // ignored upstream (didClose bails on null m_channel).
        if (RefPtr client = m_client.get()) {
            client->didReceiveMessageError("BrowserInBrowser: guest WebSocket support is not implemented"_s);
            client->didClose(0, WebCore::WebSocketChannelClient::ClosingHandshakeIncomplete,
                CloseEventCodeAbnormalClosure, "BrowserInBrowser: guest WebSocket support is not implemented"_s);
        }
        return ConnectStatus::OK;
    }

    // The connection failed at connect(); nothing below ever has work to do.
    String subprotocol() final { return String(); }
    String extensions() final { return String(); }
    void send(CString&&) final { }
    void send(const JSC::ArrayBuffer&, unsigned, unsigned) final { }
    void send(WebCore::Blob&) final { }
    void close(int, const String&) final { }
    void fail(String&&) final { }
    void disconnect() final { }
    void suspend() final { }
    void resume() final { }
    WebCore::WebSocketChannelIdentifier progressIdentifier() const final { return m_progressIdentifier; }
    bool hasCreatedHandshake() const final { return false; }
    bool isConnected() const final { return false; }
    WebCore::ResourceRequest clientHandshakeRequest(const CookieGetter&) const final { return { }; }
    const WebCore::ResourceResponse& serverHandshakeResponse() const final
    {
        static NeverDestroyed<WebCore::ResourceResponse> response;
        return response;
    }

private:
    BibFailFastWebSocketChannel(WebCore::Document& document, WebCore::WebSocketChannelClient& client)
        : m_document(document)
        , m_client(client)
        , m_progressIdentifier(WebCore::WebSocketChannelIdentifier::generate())
    {
    }

    WeakPtr<WebCore::Document, WebCore::WeakPtrImplWithEventTargetData> m_document;
    ThreadSafeWeakPtr<WebCore::WebSocketChannelClient> m_client;
    WebCore::WebSocketChannelIdentifier m_progressIdentifier;
};

class BibSocketProvider final : public WebCore::SocketProvider {
public:
    static Ref<BibSocketProvider> create() { return adoptRef(*new BibSocketProvider); }

    RefPtr<WebCore::ThreadableWebSocketChannel> createWebSocketChannel(WebCore::Document& document, WebCore::WebSocketChannelClient& client) final
    {
        return BibFailFastWebSocketChannel::create(document, client);
    }

    // Same answer as EmptySocketProvider: WebTransport is unsupported.
    std::pair<RefPtr<WebCore::WebTransportSession>, Ref<WebCore::WebTransportSessionPromise>> initializeWebTransportSession(WebCore::ScriptExecutionContext&, WebCore::WebTransportSessionClient&, const URL&, const WebCore::WebTransportOptions&) final
    {
        return { nullptr, WebCore::WebTransportSessionPromise::createAndReject() };
    }

private:
    BibSocketProvider() = default;
};

} // namespace BIB
