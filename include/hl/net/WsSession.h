// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hl/net/EventLoop.h"
#include "hl/net/WebSocketClient.h"

namespace hl {

/// WsSession options.
struct WsSessionOptions {
    std::string url{};
    WebSocketOptions ws{};
    /// Send `{"method":"ping"}` this often. HL closes connections idle for 60 s.
    std::int64_t pingIntervalMs{20'000};
    /// Reconnect if nothing at all was received for this long.
    std::int64_t staleTimeoutMs{60'000};
    /// First reconnect delay; doubles on every consecutive failure.
    std::int64_t reconnectMinDelayMs{250};
    /// Upper bound of the reconnect delay.
    std::int64_t reconnectMaxDelayMs{10'000};
};

/// Callbacks of a WsSession.
class WsSessionListener {
public:
    /// Connected; stored subscriptions have already been re-sent.
    virtual void onSessionOpen() {}
    /// A text message.
    virtual void onSessionMessage(std::string_view message) = 0;
    /// Connection lost; a reconnect is scheduled automatically.
    virtual void onSessionClosed(std::string_view /*reason*/) {}

protected:
    ~WsSessionListener() = default;
};

/**
 * @brief Self-healing Hyperliquid WebSocket connection.
 *
 * Adds to WebSocketClient: HL application-level heartbeats, stale-connection
 * detection, exponential-backoff reconnects and a subscription registry that
 * is replayed after every reconnect.
 */
class WsSession final : private WebSocketListener {
public:
    WsSession(EventLoop& loop, WsSessionListener& listener, WsSessionOptions options);
    ~WsSession();
    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

    /// Connect (and keep reconnecting until `stop()`).
    void start();
    /// Close and stop reconnecting.
    void stop();

    /**
     * @brief Register a subscription, e.g. `{"type":"l2Book","coin":"BTC"}`. Sent now if
     *        connected and replayed on every reconnect. Duplicates are ignored.
     */
    void subscribe(std::string subscriptionJson);
    /// Remove a subscription and send `unsubscribe` if connected.
    void unsubscribe(std::string_view subscriptionJson);
    /// Remove every subscription (sending `unsubscribe` for each while connected).
    void clearSubscriptions();
    [[nodiscard]] const std::vector<std::string>& subscriptions() const noexcept { return subscriptions_; }

    /// Send a raw text frame. Returns false when not connected.
    bool send(std::string_view text);

    /**
     * @brief Drop the connection now and reconnect immediately (no backoff delay).
     *
     * Reports the close to the listener exactly like a spontaneous disconnect, so subscriptions are
     * replayed and dependent state (readiness, reconciliation) is rebuilt. Useful to move to another
     * endpoint address, to recover from a suspected half-open socket, and in tests.
     */
    void reconnectNow(std::string_view reason = "manual reconnect");

    [[nodiscard]] bool isOpen() const noexcept { return ws_.isOpen(); }
    [[nodiscard]] std::uint64_t reconnectCount() const noexcept { return reconnects_; }
    [[nodiscard]] std::int64_t lastMessageMs() const noexcept { return lastRxMs_; }

private:
    void onWsOpen() override;
    void onWsText(std::string_view message) override;
    void onWsClosed(std::string_view reason) override;

    void connect();
    void scheduleReconnect();
    void armHeartbeat();
    void disarm();

    EventLoop& loop_;
    WsSessionListener& listener_;
    WsSessionOptions options_;
    WebSocketClient ws_;
    std::vector<std::string> subscriptions_;
    bool running_{false};
    std::int64_t nextDelayMs_{0};
    std::int64_t lastRxMs_{0};
    std::uint64_t reconnects_{0};
    EventLoop::TimerId reconnectTimer_{0};
    EventLoop::TimerId heartbeatTimer_{0};
};

}  // namespace hl
