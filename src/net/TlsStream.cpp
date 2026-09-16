#include "hl/net/TlsStream.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>

#include "hl/core/Log.h"

namespace hl {

namespace {

constexpr std::size_t kReadChunk = 64 * 1024;

std::string opensslError() {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return "unknown TLS error";
    }
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    ERR_clear_error();
    return buf;
}

SSL_CTX* makeContext(const TlsOptions& options) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (options.verifyPeer) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        const bool loaded = options.caFile.empty()
                                ? SSL_CTX_set_default_verify_paths(ctx) == 1
                                : SSL_CTX_load_verify_locations(ctx, options.caFile.c_str(), nullptr) == 1;
        if (!loaded) {
            logf(LogLevel::Warn, "TLS: could not load CA certificates (%s)", opensslError().c_str());
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }
    return ctx;
}

}  // namespace

TlsStream::TlsStream(EventLoop& loop, TlsStreamListener& listener, TlsOptions options)
    : loop_(loop), listener_(listener), options_(std::move(options)) {
    readBuf_.resize(kReadChunk);
}

TlsStream::~TlsStream() {
    release();
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
    }
}

bool TlsStream::connect(const std::string& host, std::uint16_t port, bool secure) {
    release();
    host_ = host;
    secure_ = secure;
    outbox_.clear();
    outboxOffset_ = 0;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result); rc != 0) {
        logf(LogLevel::Error, "resolve %s: %s", host.c_str(), ::gai_strerror(rc));
        state_ = State::Closed;
        return false;
    }
    int fd = -1;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (options_.tcpNoDelay) {
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    if (fd < 0) {
        logf(LogLevel::Error, "connect %s:%u failed: %s", host.c_str(), port, std::strerror(errno));
        state_ = State::Closed;
        return false;
    }
    fd_ = fd;
    state_ = State::Connecting;
    currentMask_ = EPOLLOUT | EPOLLIN | EPOLLRDHUP;
    loop_.add(fd_, currentMask_, this);
    connectTimer_ = loop_.addTimer(options_.connectTimeoutMs, [this] {
        connectTimer_ = 0;
        if (state_ == State::Connecting || state_ == State::Handshaking) {
            fail("connect timeout");
        }
    });
    return true;
}

bool TlsStream::send(std::string_view bytes) {
    if (state_ == State::Closed || state_ == State::Idle) {
        return false;
    }
    if (outboxOffset_ != 0 && outboxOffset_ == outbox_.size()) {
        outbox_.clear();
        outboxOffset_ = 0;
    }
    outbox_.insert(outbox_.end(), bytes.begin(), bytes.end());
    if (state_ == State::Open) {
        doWrite();
    }
    return true;
}

void TlsStream::close() {
    release();
    state_ = State::Closed;
}

void TlsStream::release() {
    if (connectTimer_ != 0) {
        loop_.cancelTimer(connectTimer_);
        connectTimer_ = 0;
    }
    if (ssl_ != nullptr) {
        if (state_ == State::Open) {
            (void)SSL_shutdown(ssl_);
        }
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        loop_.remove(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

void TlsStream::fail(std::string_view reason) {
    if (state_ == State::Closed) {
        return;
    }
    release();
    state_ = State::Closed;
    listener_.onTlsClosed(reason);
}

void TlsStream::onIoEvent(std::uint32_t events) {
    switch (state_) {
        case State::Connecting:
            if ((events & (EPOLLERR | EPOLLHUP)) != 0 && (events & EPOLLOUT) == 0) {
                fail("connection refused");
                return;
            }
            onConnectReady();
            return;
        case State::Handshaking:
            doHandshake();
            return;
        case State::Open:
            if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0) {
                doRead();
            }
            if (state_ == State::Open && (events & EPOLLOUT) != 0) {
                doWrite();
            }
            return;
        default:
            return;
    }
}

void TlsStream::onConnectReady() {
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        fail(std::string{"connect: "} + std::strerror(err));
        return;
    }
    if (!secure_) {
        if (connectTimer_ != 0) {
            loop_.cancelTimer(connectTimer_);
            connectTimer_ = 0;
        }
        state_ = State::Open;
        updateInterest();
        listener_.onTlsConnected();
        if (state_ == State::Open) {
            doWrite();
        }
        return;
    }
    if (ctx_ == nullptr) {
        ctx_ = makeContext(options_);
        if (ctx_ == nullptr) {
            fail("SSL_CTX_new failed: " + opensslError());
            return;
        }
    }
    ssl_ = SSL_new(ctx_);
    if (ssl_ == nullptr) {
        fail("SSL_new failed: " + opensslError());
        return;
    }
    SSL_set_fd(ssl_, fd_);
    // SNI (the SSL_set_tlsext_host_name macro expands to an old-style cast).
    SSL_ctrl(ssl_, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, const_cast<char*>(host_.c_str()));
    if (options_.verifyPeer) {
        SSL_set1_host(ssl_, host_.c_str());
    }
    state_ = State::Handshaking;
    doHandshake();
}

void TlsStream::doHandshake() {
    ERR_clear_error();
    const int rc = SSL_connect(ssl_);
    if (rc == 1) {
        if (connectTimer_ != 0) {
            loop_.cancelTimer(connectTimer_);
            connectTimer_ = 0;
        }
        state_ = State::Open;
        updateInterest();
        listener_.onTlsConnected();
        if (state_ == State::Open) {
            doWrite();
        }
        return;
    }
    const int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ) {
        currentMask_ = EPOLLIN | EPOLLRDHUP;
        loop_.modify(fd_, currentMask_);
    } else if (err == SSL_ERROR_WANT_WRITE) {
        currentMask_ = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        loop_.modify(fd_, currentMask_);
    } else {
        const long verify = SSL_get_verify_result(ssl_);
        std::string reason = "TLS handshake failed: " + opensslError();
        if (verify != X509_V_OK) {
            reason += std::string{" (certificate: "} + X509_verify_cert_error_string(verify) + ")";
        }
        fail(reason);
    }
}

void TlsStream::doRead() {
    for (;;) {
        ssize_t n = 0;
        if (secure_) {
            ERR_clear_error();
            const int rc = SSL_read(ssl_, readBuf_.data(), static_cast<int>(readBuf_.size()));
            if (rc <= 0) {
                const int err = SSL_get_error(ssl_, rc);
                if (err == SSL_ERROR_WANT_READ) {
                    return;
                }
                if (err == SSL_ERROR_WANT_WRITE) {
                    wantWrite_ = true;
                    updateInterest();
                    return;
                }
                if (err == SSL_ERROR_ZERO_RETURN) {
                    fail("closed by peer");
                } else if (err == SSL_ERROR_SYSCALL && errno == 0) {
                    fail("connection reset (EOF)");
                } else {
                    fail("TLS read error: " + opensslError() + " errno=" + std::strerror(errno));
                }
                return;
            }
            n = rc;
        } else {
            n = ::recv(fd_, readBuf_.data(), readBuf_.size(), 0);
            if (n == 0) {
                fail("closed by peer");
                return;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                fail(std::string{"recv: "} + std::strerror(errno));
                return;
            }
        }
        listener_.onTlsData(readBuf_.data(), static_cast<std::size_t>(n));
        if (state_ != State::Open) {
            return;  // listener closed or reconnected the stream
        }
    }
}

void TlsStream::doWrite() {
    wantWrite_ = false;
    while (outboxOffset_ < outbox_.size()) {
        const char* data = outbox_.data() + outboxOffset_;
        const std::size_t len = outbox_.size() - outboxOffset_;
        ssize_t n = 0;
        if (secure_) {
            ERR_clear_error();
            const int rc = SSL_write(ssl_, data, static_cast<int>(len));
            if (rc <= 0) {
                const int err = SSL_get_error(ssl_, rc);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                    break;
                }
                fail("TLS write error: " + opensslError());
                return;
            }
            n = rc;
        } else {
            n = ::send(fd_, data, len, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                fail(std::string{"send: "} + std::strerror(errno));
                return;
            }
        }
        outboxOffset_ += static_cast<std::size_t>(n);
    }
    if (outboxOffset_ == outbox_.size()) {
        outbox_.clear();
        outboxOffset_ = 0;
    }
    updateInterest();
}

void TlsStream::updateInterest() {
    if (fd_ < 0) {
        return;
    }
    const bool needOut = wantWrite_ || outboxOffset_ < outbox_.size();
    const std::uint32_t mask = EPOLLIN | EPOLLRDHUP | (needOut ? EPOLLOUT : 0U);
    if (mask != currentMask_) {
        currentMask_ = mask;
        loop_.modify(fd_, mask);
    }
}

}  // namespace hl
