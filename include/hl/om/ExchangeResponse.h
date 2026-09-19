// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hl/core/Decimal.h"
#include "hl/core/Result.h"
#include "hl/core/Types.h"

namespace hl {

/// Per-order (or per-cancel) outcome inside an `/exchange` response.
struct ActionStatus {
    enum class Kind : std::uint8_t {
        Resting,            ///< order accepted and resting — `oid` set
        Filled,             ///< order filled immediately — `oid`, `totalSz`, `avgPx` set
        WaitingForFill,     ///< accepted, awaiting fill (e.g. IOC in flight)
        WaitingForTrigger,  ///< trigger order accepted
        Success,            ///< cancel / modify succeeded
        Error,              ///< rejected — `error` holds the venue message
    };

    Kind kind{Kind::Error};
    std::uint64_t oid{0};
    std::optional<Cloid> cloid{};
    Decimal totalSz{};
    Decimal avgPx{};
    std::string error{};
};

/// Decoded `/exchange` response (HTTP body or WebSocket post payload).
struct ExchangeResponse {
    /// `response.type`: "order", "cancel", "batchModify", "default", …
    std::string type{};
    /// One entry per order / cancel in the request, in request order. Empty for "default".
    std::vector<ActionStatus> statuses{};
};

/**
 * @brief Parse an `/exchange` response body.
 *
 * `{"status":"ok","response":{...}}` yields an ExchangeResponse; per-order
 * rejections are *successful* parses with `ActionStatus::Kind::Error` entries.
 * `{"status":"err","response":"…"}` yields an `Error{Kind::Venue}`; malformed
 * input yields `Error{Kind::Parse}`.
 */
[[nodiscard]] Result<ExchangeResponse> parseExchangeResponse(std::string_view body);

}  // namespace hl
