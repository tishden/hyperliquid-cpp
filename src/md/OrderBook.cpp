// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/md/OrderBook.h"
#include "hl/core/Int128.h"

#include <algorithm>
#include <cstring>

namespace hl {

namespace {

std::size_t copyLevels(std::span<const BookLevel> src, BookLevel* dst) noexcept {
    const std::size_t n = std::min(src.size(), OrderBook::kMaxLevels);
    std::memcpy(static_cast<void*>(dst), src.data(), n * sizeof(BookLevel));
    return n;
}

}  // namespace

void OrderBook::applySnapshot(const L2BookMsg& msg) noexcept {
    bidCount_ = copyLevels(msg.bids, bids_.data());
    askCount_ = copyLevels(msg.asks, asks_.data());
    timeMs_ = msg.timeMs;
    snapshotTimeMs_ = msg.timeMs;
    ++updates_;
}

bool OrderBook::applyBbo(const BboMsg& msg) noexcept {
    if (msg.timeMs < snapshotTimeMs_) {
        return false;
    }
    if (msg.hasBid) {
        overlayTop(bids_, bidCount_, msg.bid, true);
    } else {
        bidCount_ = 0;
    }
    if (msg.hasAsk) {
        overlayTop(asks_, askCount_, msg.ask, false);
    } else {
        askCount_ = 0;
    }
    // Stale deeper levels that the opposite best moved through are no longer valid.
    if (msg.hasAsk) {
        dropCrossing(bids_, bidCount_, msg.ask.px, true);
    }
    if (msg.hasBid) {
        dropCrossing(asks_, askCount_, msg.bid.px, false);
    }
    timeMs_ = msg.timeMs;
    ++updates_;
    return true;
}

void OrderBook::overlayTop(std::array<BookLevel, kMaxLevels>& levels, std::size_t& count, const BookLevel& best,
                           bool isBid) noexcept {
    // Count leading levels that are better than the new best (they no longer exist).
    std::size_t drop = 0;
    while (drop < count && (isBid ? levels[drop].px > best.px : levels[drop].px < best.px)) {
        ++drop;
    }
    if (drop < count && levels[drop].px == best.px) {
        // Existing level becomes the top: shift it to index 0 and update size.
        if (drop != 0) {
            std::memmove(static_cast<void*>(levels.data()), levels.data() + drop, (count - drop) * sizeof(BookLevel));
            count -= drop;
        }
        levels[0] = best;
        return;
    }
    // New best level between (or beyond) existing ones: replace the dropped prefix with it.
    if (drop == 0) {
        const std::size_t keep = std::min(count, kMaxLevels - 1);
        std::memmove(static_cast<void*>(levels.data() + 1), levels.data(), keep * sizeof(BookLevel));
        count = keep + 1;
    } else if (drop > 1) {
        std::memmove(static_cast<void*>(levels.data() + 1), levels.data() + drop, (count - drop) * sizeof(BookLevel));
        count -= drop - 1;
    }
    levels[0] = best;
}

void OrderBook::dropCrossing(std::array<BookLevel, kMaxLevels>& levels, std::size_t& count, Decimal limit,
                             bool isBid) noexcept {
    // Levels are sorted best-first; crossing ones are at the front but level 0 is the fresh bbo,
    // so a crossing level can only appear if the bbo itself is crossed — trim from the back of
    // the valid region instead: remove every level on the wrong side of `limit`.
    std::size_t out = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const bool crosses = isBid ? levels[i].px >= limit : levels[i].px <= limit;
        if (!crosses) {
            levels[out++] = levels[i];
        }
    }
    count = out;
}

void OrderBook::clear() noexcept {
    bidCount_ = 0;
    askCount_ = 0;
}

Decimal OrderBook::mid() const noexcept {
    if (!isValid()) {
        return {};
    }
    return Decimal::fromRaw((bids_[0].px.raw() + asks_[0].px.raw()) / 2);
}

Decimal OrderBook::spread() const noexcept { return isValid() ? asks_[0].px - bids_[0].px : Decimal{}; }

double OrderBook::spreadBps() const noexcept {
    if (!isValid()) {
        return 0.0;
    }
    return spread().toDouble() / mid().toDouble() * 10'000.0;
}

Decimal OrderBook::microprice() const noexcept {
    if (!isValid()) {
        return {};
    }
    const Decimal totalSz = bids_[0].sz + asks_[0].sz;
    if (totalSz.isZero()) {
        return mid();
    }
    const Int128 num = static_cast<Int128>(bids_[0].px.raw()) * asks_[0].sz.raw() +
                         static_cast<Int128>(asks_[0].px.raw()) * bids_[0].sz.raw();
    return Decimal::fromRaw(static_cast<std::int64_t>(num / totalSz.raw()));
}

Decimal OrderBook::cumulativeSize(Side s, std::size_t levels) const noexcept {
    const auto lv = side(s);
    Decimal total;
    for (std::size_t i = 0; i < std::min(levels, lv.size()); ++i) {
        total += lv[i].sz;
    }
    return total;
}

std::optional<Decimal> OrderBook::vwapForSize(Side takerSide, Decimal sz) const noexcept {
    if (sz.raw() <= 0) {
        return std::nullopt;
    }
    const auto lv = takerSide == Side::Buy ? asks() : bids();
    Decimal remaining = sz;
    Int128 notional = 0;
    for (const auto& level : lv) {
        const Decimal take = std::min(remaining, level.sz);
        notional += static_cast<Int128>(take.raw()) * level.px.raw();
        remaining -= take;
        if (remaining.isZero()) {
            return Decimal::fromRaw(static_cast<std::int64_t>(notional / sz.raw()));
        }
    }
    return std::nullopt;
}

}  // namespace hl
