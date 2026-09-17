#include "hl/net/WsCodec.h"

#include <openssl/sha.h>

#include <cstring>

namespace hl {

void encodeWsFrame(WsOpcode opcode, std::string_view payload, std::uint32_t maskKey, std::string& out) {
    const std::size_t len = payload.size();
    const std::size_t start = out.size();
    std::size_t header = 2 + 4;
    if (len >= 126 && len <= 0xFFFF) {
        header += 2;
    } else if (len > 0xFFFF) {
        header += 8;
    }
    out.resize(start + header + len);
    auto* p = reinterpret_cast<std::uint8_t*>(out.data() + start);
    *p++ = static_cast<std::uint8_t>(0x80 | static_cast<std::uint8_t>(opcode));
    if (len < 126) {
        *p++ = static_cast<std::uint8_t>(0x80 | len);
    } else if (len <= 0xFFFF) {
        *p++ = 0x80 | 126;
        *p++ = static_cast<std::uint8_t>(len >> 8);
        *p++ = static_cast<std::uint8_t>(len);
    } else {
        *p++ = 0x80 | 127;
        for (int i = 7; i >= 0; --i) {
            *p++ = static_cast<std::uint8_t>(static_cast<std::uint64_t>(len) >> (8 * i));
        }
    }
    const std::uint8_t mask[4] = {static_cast<std::uint8_t>(maskKey >> 24), static_cast<std::uint8_t>(maskKey >> 16),
                                  static_cast<std::uint8_t>(maskKey >> 8), static_cast<std::uint8_t>(maskKey)};
    std::memcpy(p, mask, 4);
    p += 4;
    // Mask 8 bytes at a time: the 4-byte key repeated twice as one 64-bit word.
    const auto* src = reinterpret_cast<const std::uint8_t*>(payload.data());
    std::uint64_t mask64 = 0;
    std::memcpy(&mask64, mask, 4);
    std::memcpy(reinterpret_cast<std::uint8_t*>(&mask64) + 4, mask, 4);
    std::size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, src + i, 8);
        word ^= mask64;
        std::memcpy(p + i, &word, 8);
    }
    for (; i < len; ++i) {
        p[i] = src[i] ^ mask[i & 3];
    }
}

void WsFrameDecoder::reset() noexcept {
    ++epoch_;
    buffer_.clear();
    consumed_ = 0;
    fragments_.clear();
    inFragmentedMessage_ = false;
}

WsFrameDecoder::Status WsFrameDecoder::feed(const char* data, std::size_t len, WsFrameSink& sink) {
    buffer_.insert(buffer_.end(), data, data + len);
    const std::uint64_t epoch = epoch_;
    Status status = Status::Ok;
    for (;;) {
        const std::size_t avail = buffer_.size() - consumed_;
        if (avail < 2) {
            break;
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(buffer_.data() + consumed_);
        const bool fin = (p[0] & 0x80) != 0;
        const auto opcode = static_cast<WsOpcode>(p[0] & 0x0F);
        const bool masked = (p[1] & 0x80) != 0;
        std::uint64_t payloadLen = p[1] & 0x7F;
        std::size_t header = 2;
        if (payloadLen == 126) {
            if (avail < 4) {
                break;
            }
            payloadLen = (static_cast<std::uint64_t>(p[2]) << 8) | p[3];
            header = 4;
        } else if (payloadLen == 127) {
            if (avail < 10) {
                break;
            }
            payloadLen = 0;
            for (int i = 0; i < 8; ++i) {
                payloadLen = (payloadLen << 8) | p[2 + i];
            }
            header = 10;
        }
        if (masked) {
            header += 4;  // servers must not mask, but tolerate it
        }
        if (payloadLen > maxMessage_) {
            status = Status::MessageTooBig;
            break;
        }
        if (avail < header + payloadLen) {
            break;
        }
        auto* payload = buffer_.data() + consumed_ + header;
        if (masked) {
            const std::uint8_t* key = p + header - 4;
            for (std::uint64_t i = 0; i < payloadLen; ++i) {
                payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i & 3]);
            }
        }
        const std::string_view body{payload, static_cast<std::size_t>(payloadLen)};
        consumed_ += header + static_cast<std::size_t>(payloadLen);

        const auto op = static_cast<std::uint8_t>(opcode);
        if (op >= 0x8) {
            if (!fin || payloadLen > 125) {
                status = Status::ProtocolError;
                break;
            }
            sink.onWsControl(opcode, body);
            if (epoch != epoch_) {
                return Status::Ok;  // reset() from inside the callback
            }
        } else if (opcode == WsOpcode::Continuation) {
            if (!inFragmentedMessage_) {
                status = Status::ProtocolError;
                break;
            }
            if (fragments_.size() + body.size() > maxMessage_) {
                status = Status::MessageTooBig;
                break;
            }
            fragments_.append(body);
            if (fin) {
                inFragmentedMessage_ = false;
                const std::string message = std::move(fragments_);
                fragments_.clear();
                sink.onWsMessage(fragmentOpcode_, message);
                if (epoch != epoch_) {
                    return Status::Ok;
                }
            }
        } else if (opcode == WsOpcode::Text || opcode == WsOpcode::Binary) {
            if (inFragmentedMessage_) {
                status = Status::ProtocolError;
                break;
            }
            if (fin) {
                sink.onWsMessage(opcode, body);
            if (epoch != epoch_) {
                return Status::Ok;  // reset() from inside the callback
            }
            } else {
                inFragmentedMessage_ = true;
                fragmentOpcode_ = opcode;
                fragments_.assign(body);
            }
        } else {
            status = Status::ProtocolError;
            break;
        }
    }
    // Compact: drop consumed bytes once they dominate the buffer.
    if (consumed_ == buffer_.size()) {
        buffer_.clear();
        consumed_ = 0;
    } else if (consumed_ > 64 * 1024 && consumed_ * 2 > buffer_.size()) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
        consumed_ = 0;
    }
    return status;
}

std::string base64Encode(const std::uint8_t* data, std::size_t len) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back(kTable[v & 63]);
    }
    if (i < len) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) {
            v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? kTable[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string wsAcceptKey(std::string_view key) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string input{key};
    input += kGuid;
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    return base64Encode(digest, SHA_DIGEST_LENGTH);
}

}  // namespace hl
