#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "hl/net/EventLoop.h"
#include "hl/net/WsCodec.h"

namespace hltest {

/**
 * In-process fake Hyperliquid venue on 127.0.0.1 (plain TCP): serves HTTP
 * POST requests and WebSocket connections on the same port, driven by the
 * test's EventLoop. Behaviour is scripted through the public callbacks.
 */
class MockVenue final : private hl::IoHandler {
public:
    struct HttpReply {
        int status{200};
        std::string body{};
    };

    explicit MockVenue(hl::EventLoop& loop);
    ~MockVenue();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::string httpUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    [[nodiscard]] std::string wsUrl() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/ws"; }

    /// (path, body) → reply. Default: 404.
    std::function<HttpReply(const std::string& path, const std::string& body)> onHttp;
    /// (connection id, text) for every WebSocket text message. Default: none.
    std::function<void(int conn, const std::string& text)> onWsText;
    /// Called when a WebSocket handshake completes.
    std::function<void(int conn)> onWsOpen;

    void wsSend(int conn, std::string_view text);
    void wsBroadcast(std::string_view text);
    /// Abruptly close every WebSocket connection.
    void dropWebSockets();

    [[nodiscard]] int openWebSockets() const;
    std::vector<std::pair<std::string, std::string>> httpLog;  ///< (path, body)
    std::vector<std::string> wsLog;                            ///< every text message received

private:
    struct Conn;
    void onIoEvent(std::uint32_t events) override;
    void closeConn(int fd);

    hl::EventLoop& loop_;
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::map<int, std::unique_ptr<Conn>> conns_;
};

/// Run @p loop until @p pred holds or @p timeoutMs elapses. Returns pred().
bool runUntil(hl::EventLoop& loop, const std::function<bool()>& pred, int timeoutMs = 3000);

}  // namespace hltest
