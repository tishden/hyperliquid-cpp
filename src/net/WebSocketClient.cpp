#include "hl/net/WebSocketClient.h"

#include <openssl/rand.h>

#include <cctype>
#include <cstring>

#include "hl/Version.h"
#include "hl/core/Log.h"

namespace hl {

namespace {

bool headerEquals(std::string_view headers, std::string_view name, std::string_view expected) {
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const std::size_t eol = headers.find("\r\n", pos);
        const std::string_view line = headers.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        const std::size_t colon = line.find(':');
        if (colon == name.size()) {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') {
                    value.remove_prefix(1);
                }
                while (!value.empty() && value.back() == ' ') {
                    value.remove_suffix(1);
                }
                return value == expected;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = eol + 2;
    }
    return false;
}

}  // namespace

WebSocketClient::WebSocketClient(EventLoop& loop, WebSocketListener& listener, WebSocketOptions options)
    : listener_(listener),
      options_(std::move(options)),
      stream_(loop, *this, options_.tls),
      decoder_(options_.maxMessageBytes) {
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&rng_), sizeof(rng_)) != 1 || rng_ == 0) {
        rng_ = 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint64_t>(EventLoop::wallClockMs());
    }
}

WebSocketClient::~WebSocketClient() { stream_.close(); }

std::uint32_t WebSocketClient::nextMask() noexcept {
    // xorshift64* — masking keys need not be cryptographically strong (RFC 6455 §10.3
    // guards against proxy cache poisoning; the TLS channel already prevents that).
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    return static_cast<std::uint32_t>((rng_ * 0x2545F4914F6CDD1DULL) >> 32);
}

bool WebSocketClient::connect(const std::string& url) {
    auto parsed = Url::parse(url);
    if (!parsed || (parsed->scheme != "ws" && parsed->scheme != "wss")) {
        logf(LogLevel::Error, "WebSocket: invalid URL '%s'", url.c_str());
        return false;
    }
    url_ = *parsed;
    decoder_.reset();
    upgradeBuffer_.clear();
    state_ = State::Connecting;
    if (!stream_.connect(url_.host, url_.port, url_.secure)) {
        state_ = State::Closed;
        return false;
    }
    return true;
}

void WebSocketClient::onTlsConnected() {
    std::uint8_t keyBytes[16];
    if (RAND_bytes(keyBytes, sizeof(keyBytes)) != 1) {
        fail("RAND_bytes failed: cannot build the WebSocket handshake key");
        return;
    }
    handshakeKey_ = base64Encode(keyBytes, sizeof(keyBytes));
    std::string request;
    request.reserve(512);
    request += "GET " + url_.path + " HTTP/1.1\r\n";
    request += "Host: " + url_.host + "\r\n";
    request += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + handshakeKey_ + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    request += std::string{"User-Agent: hyperliquid-cpp/"} + hl::kVersionString + "\r\n";
    if (!options_.extraHeaders.empty()) {
        request += options_.extraHeaders + "\r\n";
    }
    request += "\r\n";
    state_ = State::Upgrading;
    stream_.send(request);
}

void WebSocketClient::onTlsData(const char* data, std::size_t len) {
    if (state_ == State::Upgrading) {
        upgradeBuffer_.append(data, len);
        if (upgradeBuffer_.size() > 64 * 1024) {
            fail("WebSocket upgrade response too large");
            return;
        }
        if (upgradeBuffer_.find("\r\n\r\n") == std::string::npos) {
            return;
        }
        handleUpgradeResponse();
        return;
    }
    if (state_ != State::Open) {
        return;
    }
    const auto status = decoder_.feed(data, len, *this);
    if (status != WsFrameDecoder::Status::Ok && state_ == State::Open) {
        fail(status == WsFrameDecoder::Status::MessageTooBig ? "WebSocket message too big" : "WebSocket protocol error");
    }
}

void WebSocketClient::handleUpgradeResponse() {
    const std::size_t headerEnd = upgradeBuffer_.find("\r\n\r\n");
    const std::string_view head{upgradeBuffer_.data(), headerEnd + 2};
    if (head.rfind("HTTP/1.1 101", 0) != 0) {
        const std::size_t eol = head.find("\r\n");
        fail("WebSocket upgrade rejected: " + std::string{head.substr(0, eol)});
        return;
    }
    if (!headerEquals(head, "Sec-WebSocket-Accept", wsAcceptKey(handshakeKey_))) {
        fail("WebSocket upgrade: bad Sec-WebSocket-Accept");
        return;
    }
    const std::string leftover = upgradeBuffer_.substr(headerEnd + 4);
    upgradeBuffer_.clear();
    state_ = State::Open;
    listener_.onWsOpen();
    if (state_ == State::Open && !leftover.empty()) {
        onTlsData(leftover.data(), leftover.size());
    }
}

void WebSocketClient::onTlsClosed(std::string_view reason) {
    if (state_ == State::Closed || state_ == State::Idle) {
        return;
    }
    state_ = State::Closed;
    listener_.onWsClosed(reason);
}

void WebSocketClient::onWsMessage(WsOpcode opcode, std::string_view payload) {
    if (opcode == WsOpcode::Text) {
        listener_.onWsText(payload);
    }
}

void WebSocketClient::onWsControl(WsOpcode opcode, std::string_view payload) {
    switch (opcode) {
        case WsOpcode::Ping:
            sendFrame(WsOpcode::Pong, payload);
            break;
        case WsOpcode::Close: {
            std::string reason = "closed by server";
            if (payload.size() >= 2) {
                const int code = (static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1]);
                reason += " (code " + std::to_string(code);
                if (payload.size() > 2) {
                    reason += ": " + std::string{payload.substr(2)};
                }
                reason += ")";
            }
            sendFrame(WsOpcode::Close, payload.substr(0, std::min<std::size_t>(payload.size(), 2)));
            fail(reason);
            break;
        }
        default:
            break;
    }
}

bool WebSocketClient::sendFrame(WsOpcode opcode, std::string_view payload) {
    if (state_ != State::Open) {
        return false;
    }
    frameScratch_.clear();
    encodeWsFrame(opcode, payload, nextMask(), frameScratch_);
    return stream_.send(frameScratch_);
}

bool WebSocketClient::sendText(std::string_view message) { return sendFrame(WsOpcode::Text, message); }

bool WebSocketClient::sendPing(std::string_view payload) { return sendFrame(WsOpcode::Ping, payload); }

void WebSocketClient::close() {
    if (state_ == State::Open) {
        const char normal[2] = {static_cast<char>(0x03), static_cast<char>(0xE8)};  // 1000
        sendFrame(WsOpcode::Close, std::string_view{normal, 2});
    }
    state_ = State::Closed;
    decoder_.reset();
    stream_.close();
}

void WebSocketClient::fail(std::string_view reason) {
    if (state_ == State::Closed) {
        return;
    }
    const std::string copy{reason};
    state_ = State::Closed;
    decoder_.reset();
    stream_.close();
    listener_.onWsClosed(copy);
}

}  // namespace hl
