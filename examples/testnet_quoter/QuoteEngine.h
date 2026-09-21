// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <optional>

#include "hl/core/Decimal.h"
#include "hl/md/OrderBook.h"
#include "hl/om/AssetRegistry.h"

namespace example {

/// Parameters of the symmetric, inventory-skewed quoting model.
struct QuoteParams {
    double halfSpreadBps{8.0};     ///< distance of each quote from fair value
    double skewBps{6.0};           ///< extra shift at full inventory (|position| == maxPositionUsd)
    hl::Decimal orderNotionalUsd{hl::Decimal::fromInt(20)};
    hl::Decimal maxPositionUsd{hl::Decimal::fromInt(100)};
    bool useMicroprice{true};      ///< fair value = microprice (else mid)
};

/// Target quotes for one decision; an empty side means "do not quote".
struct Quotes {
    hl::Decimal fair{};
    std::optional<hl::Decimal> bidPx{};
    std::optional<hl::Decimal> askPx{};
    hl::Decimal size{};
    double inventoryRatio{0.0};  ///< position notional / maxPositionUsd, clamped to [-1, 1]
};

/**
 * Pure pricing function of the example strategy (no I/O, fully unit-tested):
 *
 *   ratio  = clamp(position · fair / maxPositionUsd, −1, 1)
 *   shift  = −skewBps · ratio                     (long → quote lower, short → higher)
 *   bid    = fair · (1 − (halfSpread − shift)/1e4)  rounded down to a valid price
 *   ask    = fair · (1 + (halfSpread + shift)/1e4)  rounded up
 *
 * Quotes never cross the opposite best price (post-only would reject them),
 * the side that would increase an inventory already at the limit is dropped,
 * and size = orderNotionalUsd / fair rounded down to the asset's size step.
 */
[[nodiscard]] Quotes computeQuotes(const hl::OrderBook& book, hl::Decimal position, const hl::AssetInfo& asset,
                                   const QuoteParams& params);

/// |a − b| / b in basis points.
[[nodiscard]] double distanceBps(hl::Decimal a, hl::Decimal b) noexcept;

}  // namespace example
