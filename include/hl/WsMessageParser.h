#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "hl/WsMessages.h"

namespace hl {

/**
 * @brief Decodes Hyperliquid WebSocket frames and dispatches them to a WsMessageHandler.
 *
 * Built on simdjson On-Demand. After warm-up the parser performs **no heap
 * allocation** per message: level, trade, fill and order-update buffers are
 * reused. Prices and sizes are decoded straight from their JSON strings into
 * exact `Decimal`s (no `strtod`).
 *
 * Not thread-safe; use one parser per thread / connection.
 */
class WsMessageParser {
public:
    /// Counters for monitoring.
    struct Stats {
        std::uint64_t messages{0};
        std::uint64_t parseErrors{0};
        std::uint64_t unhandled{0};
    };

    WsMessageParser();
    ~WsMessageParser();
    WsMessageParser(const WsMessageParser&) = delete;
    WsMessageParser& operator=(const WsMessageParser&) = delete;
    WsMessageParser(WsMessageParser&&) noexcept;
    WsMessageParser& operator=(WsMessageParser&&) noexcept;

    /**
     * @brief Parse one complete text frame and invoke the matching handler method.
     * @return false if the frame is not valid JSON or lacks a `channel` (counted in stats).
     */
    bool parse(std::string_view frame, WsMessageHandler& handler);

    [[nodiscard]] const Stats& stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hl
