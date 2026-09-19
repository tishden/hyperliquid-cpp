// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hl/net/EventLoop.h"
#include "hl/net/TlsStream.h"
#include "hl/net/Url.h"
#include "hl/net/WsCodec.h"

namespace hl {

/// Callbacks of a WebSocketClient. Invoked on the event-loop thread.
class WebSocketListener {
public:
    /// Upgrade completed; the socket is ready for `sendText`.
    virtual void onWsOpen() = 0;
    /// A complete text message.
    virtual void onWsText(std::string_view message) = 0;
    /// Connection lost or failed (not called after an explicit `close()`).
    virtual void onWsClosed(std::string_view reason) = 0;

protected:
    ~WebSocketListener() = default;
};

/// WebSocket client options.
struct WebSocketOptions {
    TlsOptions tls{};
    /// Maximum size of a single (reassembled) message.
    std::size_t maxMessageBytes{16 * 1024 * 1024};
    /// Extra request headers, each `Name: value` without CRLF.
    std::string extraHeaders{};
};

/**
 * @brief RFC 6455 WebSocket client over TlsStream (text messages, client masking,
 *        fragmentation, automatic pong replies, close handshake).
 *
 * Reconnection policy lives in the higher-level clients; this class reports
 * a close exactly once per connection attempt.
 */
class WebSocketClient final : private TlsStreamListener, private WsFrameSink {
public:
    WebSocketClient(EventLoop& loop, WebSocketListener& listener, WebSocketOptions options = {});
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    /// Connect to `ws://` or `wss://` @p url. Returns false on immediate failure.
    bool connect(const std::string& url);
    /// Send a text message. Returns false when not open.
    bool sendText(std::string_view message);
    /// Send a ping control frame.
    bool sendPing(std::string_view payload = {});
    /// Send a close frame and drop the connection without a callback.
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return state_ == State::Open; }
    /// Bytes queued for sending (back-pressure indicator).
    [[nodiscard]] std::size_t pendingBytes() const noexcept { return stream_.pendingBytes(); }

private:
    enum class State : std::uint8_t { Idle, Connecting, Upgrading, Open, Closed };

    void onTlsConnected() override;
    void onTlsData(const char* data, std::size_t len) override;
    void onTlsClosed(std::string_view reason) override;
    void onWsMessage(WsOpcode opcode, std::string_view payload) override;
    void onWsControl(WsOpcode opcode, std::string_view payload) override;

    bool sendFrame(WsOpcode opcode, std::string_view payload);
    void handleUpgradeResponse();
    void fail(std::string_view reason);
    std::uint32_t nextMask() noexcept;

    WebSocketListener& listener_;
    WebSocketOptions options_;
    TlsStream stream_;
    WsFrameDecoder decoder_;
    Url url_{};
    State state_{State::Idle};
    std::string handshakeKey_;
    std::string upgradeBuffer_;
    std::string frameScratch_;
    std::uint64_t rng_{0};
};

}  // namespace hl
