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
    /// Pause the queue for this long after a 429 without a `Retry-After` header.
    std::int64_t defaultRateLimitPauseMs{1'000};
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
    /// Number of 429 responses seen (the queue is paused for `Retry-After` after each).
    [[nodiscard]] std::uint64_t rateLimitHits() const noexcept { return rateLimitHits_; }
    /// Monotonic time until which sending is paused after a 429 (0 = not paused).
    [[nodiscard]] std::int64_t pausedUntilMs() const noexcept { return pausedUntilMs_; }
    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

private:
    struct Request {
        std::string wire;
        Callback callback;
        bool written{false};
        std::uint8_t connectAttempts{0};  ///< connection attempts made for this request
    };

    void onTlsConnected() override;
    void onTlsData(const char* data, std::size_t len) override;
    /// Decode one chunk of response bytes; completing a request invokes its callback.
    void processBytes(const char* data, std::size_t len);
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
    bool dispatching_{false};   ///< inside a response callback
    std::string staged_;        ///< bytes that arrived while a callback was running
    std::uint64_t rateLimitHits_{0};
    std::int64_t pausedUntilMs_{0};
    EventLoop::TimerId pauseTimer_{0};
    EventLoop::TimerId requestTimer_{0};
    EventLoop::TimerId idleTimer_{0};
};

}  // namespace hl
