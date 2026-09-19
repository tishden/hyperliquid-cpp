// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include "testnet_quoter/QuoteEngine.h"

#include <algorithm>
#include <cmath>

namespace example {

using hl::Decimal;
using hl::RoundingMode;

double distanceBps(Decimal a, Decimal b) noexcept {
    if (b.isZero()) {
        return 0.0;
    }
    return std::fabs((a - b).toDouble()) / std::fabs(b.toDouble()) * 10'000.0;
}

Quotes computeQuotes(const hl::OrderBook& book, Decimal position, const hl::AssetInfo& asset, const QuoteParams& p) {
    Quotes q;
    if (!book.isValid()) {
        return q;
    }
    q.fair = p.useMicroprice ? book.microprice() : book.mid();
    const double fair = q.fair.toDouble();
    const double maxPos = p.maxPositionUsd.toDouble();
    const double positionUsd = position.toDouble() * fair;
    q.inventoryRatio = maxPos > 0 ? std::clamp(positionUsd / maxPos, -1.0, 1.0) : 0.0;

    const double shiftBps = -p.skewBps * q.inventoryRatio;
    const double bidRaw = fair * (1.0 - (p.halfSpreadBps - shiftBps) / 10'000.0);
    const double askRaw = fair * (1.0 + (p.halfSpreadBps + shiftBps) / 10'000.0);

    Decimal bid = asset.roundPx(Decimal::fromDouble(bidRaw), RoundingMode::Down);
    Decimal ask = asset.roundPx(Decimal::fromDouble(askRaw), RoundingMode::Up);
    const Decimal bestBid = book.bestBid()->px;
    const Decimal bestAsk = book.bestAsk()->px;
    if (bid >= bestAsk) {
        bid = bestBid;  // join the best bid rather than cross
    }
    if (ask <= bestBid) {
        ask = bestAsk;
    }

    q.size = asset.roundSz(p.orderNotionalUsd.div(q.fair), RoundingMode::Down);
    if (q.size.raw() <= 0) {
        return q;
    }
    if (positionUsd < maxPos) {
        q.bidPx = bid;
    }
    if (positionUsd > -maxPos) {
        q.askPx = ask;
    }
    return q;
}

}  // namespace example
