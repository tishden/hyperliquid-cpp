// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hl/WsMessages.h"
#include "hl/core/Decimal.h"
#include "hl/core/Types.h"

namespace hl {

/// An execution of one of the account's orders (owning copy of FillMsg).
struct Fill {
    std::string coin{};
    Side side{};
    Decimal px{};
    Decimal sz{};
    std::int64_t timeMs{};
    std::uint64_t oid{};
    std::uint64_t tid{};
    std::optional<Cloid> cloid{};
    bool crossed{false};        ///< true = taker
    Decimal fee{};              ///< negative = rebate
    std::string feeToken{};
    Decimal closedPnl{};
    Decimal startPosition{};    ///< signed position before the fill
    std::string dir{};
    std::string hash{};

    [[nodiscard]] static Fill from(const FillMsg& msg);
    /// Signed position after the fill.
    [[nodiscard]] Decimal endPosition() const noexcept { return side == Side::Buy ? startPosition + sz : startPosition - sz; }
};

/// A resting order as returned by `frontendOpenOrders` / `orderStatus`.
struct OpenOrder {
    std::string coin{};
    Side side{};
    Decimal limitPx{};
    Decimal sz{};          ///< remaining
    Decimal origSz{};
    std::uint64_t oid{};
    std::int64_t timestampMs{};
    std::optional<Cloid> cloid{};
    bool reduceOnly{false};
    bool isTrigger{false};
    Decimal triggerPx{};
    std::string orderType{};  ///< "Limit", "Stop Market", …
    std::string tif{};        ///< "Alo", "Ioc", "Gtc" or empty for trigger orders
};

/// Result of an `orderStatus` query.
struct OrderStatusInfo {
    bool found{false};                 ///< false = venue does not know the oid / cloid
    OpenOrder order{};
    OrderUpdateStatus status{OrderUpdateStatus::Unknown};
    std::string statusText{};
    std::int64_t statusTimestampMs{};
};

/// One open perp position from `clearinghouseState`.
struct Position {
    std::string coin{};
    Decimal szi{};              ///< signed size (positive = long)
    Decimal entryPx{};
    Decimal positionValue{};
    Decimal unrealizedPnl{};
    Decimal returnOnEquity{};
    std::optional<Decimal> liquidationPx{};
    Decimal marginUsed{};
    std::uint32_t leverage{};
    bool isCross{true};
};

/// Perp margin account summary (`clearinghouseState`).
/**
 * @brief Perp clearinghouse state (`clearinghouseState`).
 *
 * On a **unified account** — Hyperliquid's default mode, where one USDC balance backs spot and
 * perps together — `accountValue` is only the collateral currently allocated to perp positions,
 * and it reads zero while flat. It is *not* the account's equity and must not be used as buying
 * power: the spendable balance is the spot USDC balance (`SpotBalance`, `InfoClient::spotBalances`,
 * or `ExchangeConfig::loadSpotAssets`). `positions` is reported correctly in every mode.
 */
struct AccountState {
    Decimal accountValue{};
    Decimal totalNtlPos{};
    Decimal totalRawUsd{};
    Decimal totalMarginUsed{};
    Decimal withdrawable{};
    std::vector<Position> positions{};
    std::int64_t timeMs{};
};

/// One spot token balance (`spotClearinghouseState`).
struct SpotBalance {
    std::string coin{};      ///< token name, e.g. "USDC", "PURR"
    std::uint32_t token{};   ///< token index
    Decimal total{};         ///< total balance
    Decimal hold{};          ///< amount reserved by resting orders
    Decimal entryNtl{};      ///< notional at entry
};

/// Account rate-limit status (`userRateLimit`): HL grants request budget for traded volume.
struct RateLimitStatus {
    Decimal cumVlm{};                ///< cumulative traded volume in USDC
    std::uint64_t requestsUsed{0};
    std::uint64_t requestsCap{0};    ///< 10 000 + 1 per USDC of volume
    [[nodiscard]] std::uint64_t remaining() const noexcept {
        return requestsCap > requestsUsed ? requestsCap - requestsUsed : 0;
    }
};

/// One OHLCV candle (`candleSnapshot` / `candle` subscription).
struct Candle {
    std::string coin{};
    std::string interval{};     ///< "1m", "15m", "1h", "1d", …
    std::int64_t openTimeMs{};
    std::int64_t closeTimeMs{};
    Decimal open{}, close{}, high{}, low{};
    Decimal volume{};           ///< base-asset volume
    std::uint64_t trades{0};
};

/// One historical funding rate of a coin (`fundingHistory`).
struct FundingRate {
    std::string coin{};
    Decimal rate{};             ///< hourly rate
    Decimal premium{};
    std::int64_t timeMs{};
};

/// Funding rate predicted by another venue for the same coin (`predictedFundings`).
struct PredictedFunding {
    std::string coin{};
    std::string venue{};        ///< "HlPerp", "BinPerp", "BybitPerp", …
    Decimal rate{};
    std::int64_t nextFundingTimeMs{};
    int intervalHours{0};
};

/// Funding actually paid or received by the account (`userFunding`).
struct FundingPayment {
    std::int64_t timeMs{};
    std::string coin{};
    Decimal usdc{};             ///< negative = paid
    Decimal szi{};              ///< signed position at the time
    Decimal rate{};
    std::string hash{};
};

/// Market state of one perp (`metaAndAssetCtxs`), metadata and context joined.
struct PerpContext {
    std::string coin{};
    std::uint32_t asset{};
    int szDecimals{};
    std::uint32_t maxLeverage{};
    Decimal funding{};          ///< current hourly funding rate
    Decimal openInterest{};
    Decimal premium{};
    Decimal oraclePx{};
    Decimal markPx{};
    Decimal midPx{};
    Decimal prevDayPx{};
    Decimal dayNtlVlm{};        ///< 24 h notional volume
    Decimal dayBaseVlm{};
    Decimal impactBid{};        ///< impact prices used for the premium
    Decimal impactAsk{};
};

/// Result of a `userRole` query: how the venue classifies an address.
struct UserRole {
    /// "user", "agent", "vault", "subAccount", "missing", …
    std::string role{};
    /// For `role == "agent"`: the master account the agent acts for.
    std::optional<Address> master{};

    [[nodiscard]] bool isAgent() const noexcept { return role == "agent"; }
};

/// One-shot L2 snapshot (`l2Book` info request).
struct L2Snapshot {
    std::string coin{};
    std::int64_t timeMs{};
    std::vector<BookLevel> bids{};
    std::vector<BookLevel> asks{};
};

}  // namespace hl
