// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hl/net/EventLoop.h"

struct ssl_st;
struct ssl_ctx_st;

namespace hl {

/// TLS and socket options shared by all network clients.
struct TlsOptions {
    /// Verify the server certificate chain and host name (strongly recommended).
    bool verifyPeer{true};
    /// PEM CA bundle path. Empty = OpenSSL default paths (honours `SSL_CERT_FILE` / `SSL_CERT_DIR`).
    std::string caFile{};
    /// Abort a connect (TCP + TLS handshake) that takes longer than this. When a host resolves to
    /// several addresses, the budget is split across them (at least 1.5 s each) and the next address
    /// is tried after a refusal or timeout.
    std::int64_t connectTimeoutMs{10'000};
    /// Set TCP_NODELAY (disable Nagle) — on by default for request/response latency.
    bool tcpNoDelay{true};
    /**
     * Fail the connection when this many bytes are waiting to be written (0 = unbounded).
     * Protects against unbounded memory growth when a peer stops reading: the stream is closed and
     * the owner reconnects, which is what a stalled venue connection needs anyway.
     */
    std::size_t maxOutboxBytes{8 * 1024 * 1024};
};

/// Callbacks of a TlsStream. Invoked on the event-loop thread.
class TlsStreamListener {
public:
    /// TCP connected and TLS handshake completed.
    virtual void onTlsConnected() = 0;
    /// Decrypted bytes arrived.
    virtual void onTlsData(const char* data, std::size_t len) = 0;
    /// Connection failed or closed (not called for an explicit `close()`).
    virtual void onTlsClosed(std::string_view reason) = 0;

protected:
    ~TlsStreamListener() = default;
};

/**
 * @brief Non-blocking TCP (+ optional TLS) client stream driven by an EventLoop.
 *
 * Name resolution uses `getaddrinfo` and blocks briefly at `connect()`; all
 * subsequent I/O is non-blocking. Plain TCP is used when `secure == false`
 * (handy for local test servers).
 */
class TlsStream final : private IoHandler {
public:
    enum class State : std::uint8_t { Idle, Connecting, Handshaking, Open, Closed };

    TlsStream(EventLoop& loop, TlsStreamListener& listener, TlsOptions options = {});
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    /// Start connecting. Returns false on immediate failure (bad host, socket error).
    bool connect(const std::string& host, std::uint16_t port, bool secure);

    /// Queue bytes for sending; flushed as soon as the socket is writable.
    bool send(std::string_view bytes);

    /// Close immediately without invoking `onTlsClosed`.
    void close();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool isOpen() const noexcept { return state_ == State::Open; }
    /// Bytes queued but not yet written to the socket.
    [[nodiscard]] std::size_t pendingBytes() const noexcept { return outbox_.size() - outboxOffset_; }

private:
    void onIoEvent(std::uint32_t events) override;
    bool startAttempt(std::string reason);
    void nextAddress(std::string_view reason);
    void closeSocket() noexcept;
    void onConnectReady();
    void doHandshake();
    void doRead();
    void doWrite();
    void updateInterest();
    void fail(std::string_view reason);
    void release();

    EventLoop& loop_;
    TlsStreamListener& listener_;
    TlsOptions options_;
    int fd_{-1};
    bool secure_{true};
    bool wantWrite_{false};
    bool reading_{false};  // guards re-entry from a listener that pumps the event loop
    std::uint32_t currentMask_{0};
    ::ssl_ctx_st* ctx_{nullptr};
    ::ssl_st* ssl_{nullptr};
    State state_{State::Idle};
    std::string host_;
    std::uint16_t port_{0};
    /// One resolved endpoint (a sockaddr_storage blob plus its length and family).
    struct Endpoint {
        std::array<char, 128> address{};
        std::uint32_t length{0};
        int family{0};
    };
    std::vector<Endpoint> endpoints_;
    std::size_t addrCursor_{0};
    std::size_t attemptsLeft_{0};
    std::size_t rotation_{0};
    std::vector<char> outbox_;
    std::size_t outboxOffset_{0};
    std::vector<char> readBuf_;
    EventLoop::TimerId connectTimer_{0};
};

}  // namespace hl
