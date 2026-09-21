// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/net/HttpClient.h"

#include <stdexcept>

#include "hl/Version.h"
#include "hl/core/Log.h"

namespace hl {

HttpClient::HttpClient(EventLoop& loop, std::string baseUrl, HttpClientOptions options)
    : loop_(loop), baseUrl_(std::move(baseUrl)), options_(std::move(options)), stream_(loop, *this, options_.tls) {
    auto parsed = Url::parse(baseUrl_);
    if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) {
        throw std::invalid_argument("hl::HttpClient: invalid base URL " + baseUrl_);
    }
    url_ = *parsed;
    if (url_.path == "/") {
        url_.path.clear();
    }
}

HttpClient::~HttpClient() {
    disarmTimers();
    stream_.close();
}

void HttpClient::postJson(std::string_view path, std::string body, Callback callback) {
    Request req;
    req.wire.reserve(body.size() + 256);
    req.wire += "POST ";
    req.wire += url_.path;
    req.wire += path;
    req.wire += " HTTP/1.1\r\nHost: ";
    req.wire += url_.host;
    req.wire += "\r\nUser-Agent: hyperliquid-cpp/";
    req.wire += kVersionString;
    req.wire += "\r\nAccept: application/json\r\nContent-Type: application/json\r\n"
                "Connection: keep-alive\r\nContent-Length: ";
    req.wire += std::to_string(body.size());
    req.wire += "\r\n\r\n";
    req.wire += body;
    req.callback = std::move(callback);
    queue_.push_back(std::move(req));
    pump();
}

void HttpClient::pump() {
    if (queue_.empty()) {
        armIdleTimer();
        return;
    }
    if (!stream_.isOpen()) {
        if (!connecting_) {
            connecting_ = true;
            parser_.reset();
            if (!stream_.connect(url_.host, url_.port, url_.secure)) {
                connecting_ = false;
                completeHead(Error{Error::Kind::Transport, 0, "connect to " + url_.host + " failed"}, {});
            }
        }
        return;
    }
    Request& head = queue_.front();
    if (head.written) {
        return;  // awaiting response
    }
    if (pausedUntilMs_ != 0) {
        const std::int64_t remaining = pausedUntilMs_ - EventLoop::nowMs();
        if (remaining > 0) {
            if (pauseTimer_ == 0) {
                pauseTimer_ = loop_.addTimer(remaining, [this] {
                    pauseTimer_ = 0;
                    pausedUntilMs_ = 0;
                    pump();
                });
            }
            return;  // rate-limited: hold the queue instead of hammering the venue
        }
        pausedUntilMs_ = 0;
    }
    if (idleTimer_ != 0) {
        loop_.cancelTimer(idleTimer_);
        idleTimer_ = 0;
    }
    head.written = true;
    parser_.reset();
    stream_.send(head.wire);
    requestTimer_ = loop_.addTimer(options_.requestTimeoutMs, [this] {
        requestTimer_ = 0;
        // Unknown outcome: drop the connection so a late response cannot be misattributed.
        stream_.close();
        completeHead(Error{Error::Kind::Timeout, 0, "HTTP request timed out"}, {});
    });
}

void HttpClient::completeHead(const Error& error, const HttpResponse& response) {
    if (requestTimer_ != 0) {
        loop_.cancelTimer(requestTimer_);
        requestTimer_ = 0;
    }
    if (queue_.empty()) {
        return;
    }
    Request req = std::move(queue_.front());
    queue_.pop_front();
    req.callback(error, response);
    pump();
}

void HttpClient::onTlsConnected() {
    connecting_ = false;
    pump();
}

void HttpClient::onTlsData(const char* data, std::size_t len) {
    if (dispatching_) {
        // Arrived while a response callback was running (e.g. the callback pumped the event loop):
        // stage it, the outer call decodes it once the callback returns.
        staged_.append(data, len);
        return;
    }
    dispatching_ = true;
    processBytes(data, len);
    while (!staged_.empty()) {
        const std::string chunk = std::move(staged_);
        staged_.clear();
        processBytes(chunk.data(), chunk.size());
    }
    dispatching_ = false;
}

void HttpClient::processBytes(const char* data, std::size_t len) {
    while (len != 0) {
        if (queue_.empty() || !queue_.front().written) {
            logf(LogLevel::Warn, "HTTP %s: unsolicited %zu bytes dropped", url_.host.c_str(), len);
            stream_.close();
            pump();
            return;
        }
        std::size_t consumed = 0;
        const auto status = parser_.feed(data, len, consumed);
        if (status == HttpResponseParser::Status::Error) {
            const std::string reason = parser_.error();
            stream_.close();
            completeHead(Error{Error::Kind::Parse, 0, "HTTP parse error: " + reason}, {});
            return;
        }
        if (status == HttpResponseParser::Status::NeedMore) {
            return;
        }
        HttpResponse response = parser_.takeResponse();
        parser_.reset();
        data += consumed;
        len -= consumed;
        if (!response.keepAlive) {
            stream_.close();
        }
        Error error;
        if (response.status < 200 || response.status >= 300) {
            error = Error{Error::Kind::Http, response.status, "HTTP " + std::to_string(response.status)};
        }
        if (response.status == 429) {
            // Never replay the request (actions are not idempotent) — but stop sending for a while.
            ++rateLimitHits_;
            const std::int64_t pause = response.retryAfterSeconds > 0
                                           ? static_cast<std::int64_t>(response.retryAfterSeconds) * 1000
                                           : options_.defaultRateLimitPauseMs;
            pausedUntilMs_ = EventLoop::nowMs() + pause;
            logf(LogLevel::Warn, "http %s: rate limited, pausing the queue for %lld ms", url_.host.c_str(),
                 static_cast<long long>(pause));
        }
        completeHead(error, response);
        if (!stream_.isOpen()) {
            return;
        }
    }
}

void HttpClient::onTlsClosed(std::string_view reason) {
    connecting_ = false;
    staged_.clear();
    disarmTimers();
    if (queue_.empty()) {
        return;
    }
    Request& head = queue_.front();
    if (head.written) {
        if (parser_.finishOnClose() == HttpResponseParser::Status::Complete && parser_.response().status != 0) {
            HttpResponse response = parser_.takeResponse();
            completeHead(Error{}, response);
            return;
        }
        // The request may or may not have reached the server: report, never replay.
        completeHead(Error{Error::Kind::Transport, 0, "connection closed: " + std::string{reason}}, {});
        return;
    }
    if (head.connectAttempts >= 3) {
        completeHead(Error{Error::Kind::Transport, 0, "connect failed: " + std::string{reason}}, {});
        return;
    }
    ++head.connectAttempts;
    // Brief backoff so three attempts do not all hit a host that is briefly unavailable.
    pausedUntilMs_ = EventLoop::nowMs() + 50 * head.connectAttempts;
    pump();
}

void HttpClient::cancelAll(std::string_view reason) {
    disarmTimers();
    stream_.close();
    connecting_ = false;
    std::deque<Request> failed;
    failed.swap(queue_);
    for (auto& req : failed) {
        req.callback(Error{Error::Kind::Transport, 0, std::string{reason}}, {});
    }
}

void HttpClient::armIdleTimer() {
    if (idleTimer_ != 0 || !stream_.isOpen()) {
        return;
    }
    idleTimer_ = loop_.addTimer(options_.idleTimeoutMs, [this] {
        idleTimer_ = 0;
        if (queue_.empty()) {
            stream_.close();
        }
    });
}

void HttpClient::disarmTimers() {
    if (pauseTimer_ != 0) {
        loop_.cancelTimer(pauseTimer_);
        pauseTimer_ = 0;
    }
    if (requestTimer_ != 0) {
        loop_.cancelTimer(requestTimer_);
        requestTimer_ = 0;
    }
    if (idleTimer_ != 0) {
        loop_.cancelTimer(idleTimer_);
        idleTimer_ = 0;
    }
}

}  // namespace hl
