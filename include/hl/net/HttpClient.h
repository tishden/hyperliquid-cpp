#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

#include "hl/core/Types.h"
#include "hl/net/EventLoop.h"
#include "hl/net/HttpCodec.h"
#include "hl/net/TlsStream.h"
#include "hl/net/Url.h"

namespace hl {

/// HttpClient options.
struct HttpClientOptions {
    TlsOptions tls{};
    /// Per-request deadline, measured from when the request is written.
    std::int64_t requestTimeoutMs{10'000};
    /// Close the idle keep-alive connection after this long (HL's edge drops idle connections).
    std::int64_t idleTimeoutMs{50'000};
};

/**
 * @brief Asynchronous HTTP/1.1 client for one origin with a persistent connection.
 *
 * Requests are queued and sent one at a time over a single keep-alive
 * connection (no pipelining), which preserves submission order — important
 * for signed actions whose nonces must reach the venue in order. The
 * connection is (re)established lazily. A request that was never written
 * when a connection drops is retried transparently; a request that was
 * written is failed with `Error::Kind::Transport` (never replayed, since
 * `/exchange` actions are not idempotent).
 */
class HttpClient final : private TlsStreamListener {
public:
    /// Callback receiving the response or an error. Runs on the loop thread.
    using Callback = std::function<void(const Error& error, const HttpResponse& response)>;

    HttpClient(EventLoop& loop, std::string baseUrl, HttpClientOptions options = {});
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// Queue `POST <path>` with `Content-Type: application/json`.
    void postJson(std::string_view path, std::string body, Callback callback);

    /// Fail all queued requests and close the connection.
    void cancelAll(std::string_view reason = "canceled");

    /// Queued + in-flight requests.
    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

private:
    struct Request {
        std::string wire;
        Callback callback;
        bool written{false};
        std::uint8_t attempts{0};
    };

    void onTlsConnected() override;
    void onTlsData(const char* data, std::size_t len) override;
    void onTlsClosed(std::string_view reason) override;

    void pump();
    void completeHead(const Error& error, const HttpResponse& response);
    void armIdleTimer();
    void disarmTimers();

    EventLoop& loop_;
    std::string baseUrl_;
    Url url_{};
    HttpClientOptions options_;
    TlsStream stream_;
    HttpResponseParser parser_;
    std::deque<Request> queue_;
    bool connecting_{false};
    EventLoop::TimerId requestTimer_{0};
    EventLoop::TimerId idleTimer_{0};
};

}  // namespace hl
