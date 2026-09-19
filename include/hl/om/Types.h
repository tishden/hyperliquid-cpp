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
struct AccountState {
    Decimal accountValue{};
    Decimal totalNtlPos{};
    Decimal totalRawUsd{};
    Decimal totalMarginUsed{};
    Decimal withdrawable{};
    std::vector<Position> positions{};
    std::int64_t timeMs{};
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
