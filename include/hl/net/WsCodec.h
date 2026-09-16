#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hl {

/// WebSocket opcodes (RFC 6455 §5.2).
enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/**
 * @brief Append one client→server frame (FIN set, masked) to @p out.
 * @param maskKey 4-byte masking key (RFC 6455 requires clients to mask).
 */
void encodeWsFrame(WsOpcode opcode, std::string_view payload, std::uint32_t maskKey, std::string& out);

/// Receives complete messages from WsFrameDecoder.
class WsFrameSink {
public:
    /// A complete (possibly reassembled) text or binary message.
    virtual void onWsMessage(WsOpcode opcode, std::string_view payload) = 0;
    /// A control frame (ping / pong / close).
    virtual void onWsControl(WsOpcode opcode, std::string_view payload) = 0;

protected:
    ~WsFrameSink() = default;
};

/**
 * @brief Incremental server→client frame decoder with fragment reassembly.
 *
 * Feed raw bytes as they arrive; complete messages are delivered to the sink.
 * Unfragmented messages are delivered zero-copy (the view points into the
 * internal buffer and is valid only during the callback).
 */
class WsFrameDecoder {
public:
    explicit WsFrameDecoder(std::size_t maxMessageBytes = 16 * 1024 * 1024) : maxMessage_(maxMessageBytes) {}

    enum class Status : std::uint8_t { Ok, ProtocolError, MessageTooBig };

    /// Consume bytes. Stops early and returns an error status on a protocol violation.
    Status feed(const char* data, std::size_t len, WsFrameSink& sink);

    void reset() noexcept;

private:
    std::vector<char> buffer_;
    std::size_t consumed_{0};
    std::string fragments_;
    WsOpcode fragmentOpcode_{WsOpcode::Text};
    bool inFragmentedMessage_{false};
    std::uint64_t epoch_{0};
    std::size_t maxMessage_;
};

/// Compute `Sec-WebSocket-Accept` for a `Sec-WebSocket-Key` (base64(SHA1(key + GUID))).
[[nodiscard]] std::string wsAcceptKey(std::string_view secWebSocketKey);
/// Standard base64 encoding.
[[nodiscard]] std::string base64Encode(const std::uint8_t* data, std::size_t len);

}  // namespace hl
