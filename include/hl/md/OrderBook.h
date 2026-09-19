// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "hl/WsMessages.h"
#include "hl/core/Decimal.h"

namespace hl {

/**
 * @brief L2 order book for one coin, maintained from `l2Book` snapshots and `bbo` updates.
 *
 * Hyperliquid does not stream incremental depth diffs: every `l2Book` message
 * is a complete snapshot — 20 levels per side, or 5 on a `fast` subscription —
 * while `bbo` pushes every best-bid/offer change in between. The two snapshot
 * feeds run at very different rates; measured on mainnet BTC/ETH on 2026-09-19,
 * the 20-level feed arrives about every 5.35 s and the `fast` one about every
 * 0.54 s, against a `bbo` message every 150-180 ms. Levels below the top are
 * therefore as stale as the last snapshot: seconds, not milliseconds. The book:
 *  - replaces both sides on each snapshot (`applySnapshot`), and
 *  - overlays newer `bbo` updates on the top of book (`applyBbo`): levels that
 *    the new best price has moved through are removed, the best level is
 *    inserted or resized. Deeper levels keep their last snapshot values.
 *
 * Storage is two fixed-capacity inline arrays — the book never allocates after
 * construction. Level 0 is the best level on both sides.
 */
class OrderBook {
public:
    static constexpr std::size_t kMaxLevels = 64;

    explicit OrderBook(std::string coin = {}) : coin_(std::move(coin)) {}

    /// Replace both sides from a full `l2Book` snapshot.
    void applySnapshot(const L2BookMsg& msg) noexcept;

    /**
     * @brief Overlay a best-bid/offer update on top of the last snapshot.
     *
     * Levels the new best price moved through are dropped, the best level is inserted or resized,
     * and stale levels on the other side that would now cross are removed — so a locked or crossed
     * update resolves to a consistent book rather than a crossed one, at the cost of dropping the
     * offending levels. A side with no best price in the update is emptied, which leaves the book
     * `!isValid()` until the next snapshot. Levels are assumed sorted best-first, as the venue
     * sends them.
     *
     * @return false if the update is older than the last applied snapshot and was ignored.
     */
    bool applyBbo(const BboMsg& msg) noexcept;

    /// Remove all levels.
    void clear() noexcept;

    [[nodiscard]] const std::string& coin() const noexcept { return coin_; }

    /// Both sides present and best bid < best ask.
    [[nodiscard]] bool isValid() const noexcept {
        return bidCount_ != 0 && askCount_ != 0 && bids_[0].px < asks_[0].px;
    }

    [[nodiscard]] std::span<const BookLevel> bids() const noexcept { return {bids_.data(), bidCount_}; }
    [[nodiscard]] std::span<const BookLevel> asks() const noexcept { return {asks_.data(), askCount_}; }
    [[nodiscard]] std::span<const BookLevel> side(Side s) const noexcept { return s == Side::Buy ? bids() : asks(); }

    [[nodiscard]] std::optional<BookLevel> bestBid() const noexcept {
        return bidCount_ != 0 ? std::optional<BookLevel>{bids_[0]} : std::nullopt;
    }
    [[nodiscard]] std::optional<BookLevel> bestAsk() const noexcept {
        return askCount_ != 0 ? std::optional<BookLevel>{asks_[0]} : std::nullopt;
    }

    /// (bestBid + bestAsk) / 2, or zero when the book is not valid.
    [[nodiscard]] Decimal mid() const noexcept;
    /// bestAsk − bestBid, or zero when the book is not valid.
    [[nodiscard]] Decimal spread() const noexcept;
    /// Spread in basis points of mid, or 0 when the book is not valid.
    [[nodiscard]] double spreadBps() const noexcept;
    /// Size-weighted mid: (bidPx·askSz + askPx·bidSz) / (bidSz + askSz). Zero when not valid.
    [[nodiscard]] Decimal microprice() const noexcept;

    /// Sum of sizes of the first @p levels levels of a side.
    [[nodiscard]] Decimal cumulativeSize(Side side, std::size_t levels) const noexcept;

    /**
     * @brief Average price a taker would pay/receive to execute @p sz against visible depth.
     * @param takerSide Buy consumes asks, Sell consumes bids.
     * @return nullopt if visible depth is insufficient.
     */
    [[nodiscard]] std::optional<Decimal> vwapForSize(Side takerSide, Decimal sz) const noexcept;

    /// Exchange timestamp (ms) of the last applied snapshot or bbo.
    [[nodiscard]] std::int64_t timeMs() const noexcept { return timeMs_; }
    /// Exchange timestamp (ms) of the last applied full snapshot.
    [[nodiscard]] std::int64_t snapshotTimeMs() const noexcept { return snapshotTimeMs_; }
    /// Number of snapshots + bbo updates applied.
    [[nodiscard]] std::uint64_t updateCount() const noexcept { return updates_; }

private:
    static void overlayTop(std::array<BookLevel, kMaxLevels>& levels, std::size_t& count, const BookLevel& best,
                           bool isBid) noexcept;
    static void dropCrossing(std::array<BookLevel, kMaxLevels>& levels, std::size_t& count, Decimal limit,
                             bool isBid) noexcept;

    std::string coin_;
    std::array<BookLevel, kMaxLevels> bids_{};
    std::array<BookLevel, kMaxLevels> asks_{};
    std::size_t bidCount_{0};
    std::size_t askCount_{0};
    std::int64_t timeMs_{0};
    std::int64_t snapshotTimeMs_{0};
    std::uint64_t updates_{0};
};

}  // namespace hl
