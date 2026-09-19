// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include "hl/net/WsSession.h"

#include <algorithm>

#include "hl/core/Log.h"

namespace hl {

WsSession::WsSession(EventLoop& loop, WsSessionListener& listener, WsSessionOptions options)
    : loop_(loop), listener_(listener), options_(std::move(options)), ws_(loop, *this, options_.ws) {
    nextDelayMs_ = options_.reconnectMinDelayMs;
}

WsSession::~WsSession() { stop(); }

void WsSession::start() {
    if (running_) {
        return;
    }
    running_ = true;
    nextDelayMs_ = options_.reconnectMinDelayMs;
    connect();
}

void WsSession::stop() {
    running_ = false;
    disarm();
    ws_.close();
}

void WsSession::connect() {
    logf(LogLevel::Info, "ws: connecting to %s", options_.url.c_str());
    if (!ws_.connect(options_.url)) {
        scheduleReconnect();
    }
}

void WsSession::subscribe(std::string subscriptionJson) {
    if (std::find(subscriptions_.begin(), subscriptions_.end(), subscriptionJson) != subscriptions_.end()) {
        return;
    }
    if (ws_.isOpen()) {
        ws_.sendText(R"({"method":"subscribe","subscription":)" + subscriptionJson + "}");
    }
    subscriptions_.push_back(std::move(subscriptionJson));
}

void WsSession::unsubscribe(std::string_view subscriptionJson) {
    const auto it = std::find(subscriptions_.begin(), subscriptions_.end(), subscriptionJson);
    if (it == subscriptions_.end()) {
        return;
    }
    if (ws_.isOpen()) {
        ws_.sendText(R"({"method":"unsubscribe","subscription":)" + std::string{subscriptionJson} + "}");
    }
    subscriptions_.erase(it);
}

void WsSession::clearSubscriptions() {
    std::vector<std::string> subs;
    subs.swap(subscriptions_);  // detach first: sending must not iterate the container being emptied
    if (ws_.isOpen()) {
        for (const auto& sub : subs) {
            ws_.sendText(R"({"method":"unsubscribe","subscription":)" + sub + "}");
        }
    }
}

bool WsSession::send(std::string_view text) { return ws_.sendText(text); }

void WsSession::reconnectNow(std::string_view reason) {
    if (!running_) {
        return;
    }
    nextDelayMs_ = options_.reconnectMinDelayMs;
    const bool wasOpen = ws_.isOpen();
    ws_.close();
    if (wasOpen) {
        onWsClosed(reason);  // same path as a spontaneous close: listener, backoff, resubscribe
    } else {
        // Closing a socket that was still connecting produces no close callback, so schedule the
        // retry here — otherwise the session would stay down with no timer armed.
        disarm();
        scheduleReconnect();
    }
}

void WsSession::onWsOpen() {
    logf(LogLevel::Info, "ws: connected to %s (%zu subscriptions)", options_.url.c_str(), subscriptions_.size());
    nextDelayMs_ = options_.reconnectMinDelayMs;
    lastRxMs_ = EventLoop::nowMs();
    for (const auto& sub : subscriptions_) {
        ws_.sendText(R"({"method":"subscribe","subscription":)" + sub + "}");
    }
    armHeartbeat();
    listener_.onSessionOpen();
}

void WsSession::onWsText(std::string_view message) {
    lastRxMs_ = EventLoop::nowMs();
    listener_.onSessionMessage(message);
}

void WsSession::onWsClosed(std::string_view reason) {
    logf(LogLevel::Warn, "ws: %s closed: %.*s", options_.url.c_str(), static_cast<int>(reason.size()), reason.data());
    disarm();
    listener_.onSessionClosed(reason);
    scheduleReconnect();
}

void WsSession::scheduleReconnect() {
    if (!running_ || reconnectTimer_ != 0) {
        return;
    }
    const std::int64_t delay = nextDelayMs_;
    nextDelayMs_ = std::min(nextDelayMs_ * 2, options_.reconnectMaxDelayMs);
    reconnectTimer_ = loop_.addTimer(delay, [this] {
        reconnectTimer_ = 0;
        if (running_ && !ws_.isOpen()) {
            ++reconnects_;
            connect();
        }
    });
}

void WsSession::armHeartbeat() {
    heartbeatTimer_ = loop_.addTimer(options_.pingIntervalMs, [this] {
        heartbeatTimer_ = 0;
        if (!ws_.isOpen()) {
            return;
        }
        if (EventLoop::nowMs() - lastRxMs_ > options_.staleTimeoutMs) {
            logf(LogLevel::Warn, "ws: %s stale (no data for %lld ms), reconnecting", options_.url.c_str(),
                 static_cast<long long>(EventLoop::nowMs() - lastRxMs_));
            ws_.close();
            onWsClosed("stale connection");
            return;
        }
        ws_.sendText(R"({"method":"ping"})");
        armHeartbeat();
    });
}

void WsSession::disarm() {
    if (reconnectTimer_ != 0) {
        loop_.cancelTimer(reconnectTimer_);
        reconnectTimer_ = 0;
    }
    if (heartbeatTimer_ != 0) {
        loop_.cancelTimer(heartbeatTimer_);
        heartbeatTimer_ = 0;
    }
}

}  // namespace hl
