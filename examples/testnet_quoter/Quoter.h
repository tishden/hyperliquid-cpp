// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "hl/hyperliquid.h"
#include "testnet_quoter/QuoteEngine.h"

namespace example {

/// Runtime settings of the example market maker.
struct QuoterSettings {
    std::string coin{"ETH"};
    QuoteParams quote{};
    double requoteBps{3.0};                ///< amend a quote only if it is this far from target
    std::int64_t minRequoteIntervalMs{1'000};
    std::int64_t statusIntervalMs{5'000};
    bool deadManSwitch{false};             ///< refresh scheduleCancel(now + 90 s) every 30 s
    bool dryRun{false};                    ///< compute and print quotes, send nothing
};

/**
 * Two-sided post-only (ALO) quoter: one order per side, amended in place
 * (batchModify) when the target moves, skewed by inventory, with a hard
 * position limit. Wires a MarketDataClient (book) to an ExchangeClient (orders).
 */
class Quoter final : public hl::MarketDataListener, public hl::ExchangeListener {
public:
    Quoter(hl::EventLoop& loop, QuoterSettings settings);

    /// Attach the market-data client whose book drives quoting.
    void attach(hl::MarketDataClient& md) noexcept { md_ = &md; }
    /// Attach the order-management client (absent in dry-run mode).
    void attach(hl::ExchangeClient& exchange) noexcept { exchange_ = &exchange; }
    /// Asset metadata for dry-run mode (live mode uses ExchangeClient::assets()).
    void setAssets(hl::AssetRegistry assets) { dryRunAssets_ = std::move(assets); }

    /// Cancel all quotes and wait (up to @p timeoutMs) until none is live.
    void shutdown(std::int64_t timeoutMs);
    void printSummary() const;

    // MarketDataListener
    void onConnected() override;
    void onBookUpdate(const hl::OrderBook& book, BookUpdate kind) override;

    // Both MarketDataListener and ExchangeListener
    void onDisconnected(std::string_view reason) override;

    // ExchangeListener
    void onReady() override;
    void onOrderUpdate(const hl::Order& order) override;
    void onFill(const hl::Fill& fill) override;
    void onError(const hl::Error& error) override;

private:
    struct Slot {
        std::optional<hl::Cloid> cloid{};
        std::int64_t lastActionMs{0};
        std::int64_t backoffUntilMs{0};
        int consecutiveRejects{0};
    };

    void requote(const hl::OrderBook& book);
    void manageSide(hl::Side side, const std::optional<hl::Decimal>& target, hl::Decimal size);
    void refreshDeadManSwitch();
    void printStatus();

    hl::EventLoop& loop_;
    QuoterSettings settings_;
    hl::ExchangeClient* exchange_{nullptr};
    hl::MarketDataClient* md_{nullptr};
    hl::AssetRegistry dryRunAssets_{};
    std::array<Slot, 2> slots_{};  // [Buy, Sell]
    Quotes lastQuotes_{};
    bool haltedByRisk_{false};
    bool shuttingDown_{false};
    std::uint64_t fills_{0};
    std::uint64_t makerFills_{0};
    std::uint64_t ordersPlaced_{0};
    std::uint64_t amendments_{0};
    std::uint64_t rejects_{0};
    hl::Decimal cash_{};         ///< USDC flow from fills (sell +, buy −), fees subtracted
    hl::Decimal volumeUsd_{};
    hl::Decimal startPosition_{};
    bool positionSeeded_{false};
};

}  // namespace example
