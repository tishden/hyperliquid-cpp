#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "hl/core/Decimal.h"
#include "hl/core/Types.h"

/**
 * @file WsMessages.h
 * @brief Decoded Hyperliquid WebSocket messages.
 *
 * All `std::string_view` and `std::span` members point into parser-owned
 * buffers and are valid **only for the duration of the callback** that
 * receives the message. Copy what you need to keep.
 */

namespace hl {

/// One aggregated price level.
struct BookLevel {
    Decimal px{};
    Decimal sz{};
    std::uint32_t n{};  ///< number of resting orders at this level
};

/// `l2Book` channel — full snapshot of the top of the book (up to 20 levels per side).
struct L2BookMsg {
    std::string_view coin{};
    std::int64_t timeMs{};
    std::span<const BookLevel> bids{};  ///< best (highest) first
    std::span<const BookLevel> asks{};  ///< best (lowest) first
};

/// `bbo` channel — best bid/offer, pushed on every top-of-book change.
struct BboMsg {
    std::string_view coin{};
    std::int64_t timeMs{};
    bool hasBid{false};
    bool hasAsk{false};
    BookLevel bid{};
    BookLevel ask{};
};

/// One public trade from the `trades` channel. Liquidations are included here.
struct TradeMsg {
    std::string_view coin{};
    Side side{};           ///< aggressor side
    Decimal px{};
    Decimal sz{};
    std::int64_t timeMs{};
    std::uint64_t tid{};   ///< trade id
    std::string_view hash{};
};

/// `activeAssetCtx` / `activeSpotAssetCtx` — funding, oracle, mark, open interest, volume.
struct AssetCtxMsg {
    std::string_view coin{};
    bool isSpot{false};
    Decimal funding{};        ///< current hourly funding rate (perps)
    Decimal openInterest{};   ///< perps
    Decimal oraclePx{};       ///< perps
    Decimal premium{};        ///< perps
    Decimal markPx{};
    Decimal midPx{};
    bool hasMidPx{false};
    Decimal prevDayPx{};
    Decimal dayNtlVlm{};
    Decimal dayBaseVlm{};
    Decimal circulatingSupply{};  ///< spot
};

/// One entry of the `allMids` channel.
struct MidEntry {
    std::string_view coin{};
    Decimal mid{};
};

/// `allMids` channel — mid price of every coin.
struct AllMidsMsg {
    std::span<const MidEntry> mids{};
};

/// Order lifecycle status as reported by `orderUpdates`.
enum class OrderUpdateStatus : std::uint8_t {
    Open,       ///< resting on the book
    Filled,     ///< fully filled
    Canceled,   ///< canceled by user, scheduled cancel, margin, self-trade prevention, …
    Triggered,  ///< trigger order activated
    Rejected,   ///< rejected by the matching engine
    Unknown,    ///< status string not recognised — see `statusText`
};

[[nodiscard]] OrderUpdateStatus parseOrderUpdateStatus(std::string_view status) noexcept;
[[nodiscard]] std::string_view toString(OrderUpdateStatus status) noexcept;

/// One element of the private `orderUpdates` channel.
struct OrderUpdateMsg {
    std::string_view coin{};
    Side side{};
    Decimal limitPx{};
    Decimal sz{};         ///< remaining size
    Decimal origSz{};     ///< original size
    std::uint64_t oid{};
    std::int64_t timestampMs{};
    std::optional<Cloid> cloid{};
    OrderUpdateStatus status{OrderUpdateStatus::Unknown};
    std::string_view statusText{};  ///< raw venue status, e.g. "canceled", "reduceOnlyCanceled"
    std::int64_t statusTimestampMs{};
};

/// One of the account's own executions (`userFills` channel).
struct FillMsg {
    std::string_view coin{};
    Decimal px{};
    Decimal sz{};
    Side side{};
    std::int64_t timeMs{};
    Decimal startPosition{};    ///< signed position before this fill
    std::string_view dir{};     ///< "Open Long", "Close Short", "Buy", …
    Decimal closedPnl{};
    std::string_view hash{};
    std::uint64_t oid{};
    bool crossed{false};        ///< true = taker
    Decimal fee{};              ///< negative = rebate
    std::string_view feeToken{};
    std::uint64_t tid{};
    std::optional<Cloid> cloid{};
};

/// `userFills` channel batch. The first message after subscribing is a snapshot of recent fills.
struct UserFillsMsg {
    bool isSnapshot{false};
    std::string_view user{};
    std::span<const FillMsg> fills{};
};

/// Response to a `{"method":"post"}` request (action or info) sent over the WebSocket.
struct PostResponseMsg {
    enum class Type : std::uint8_t { Action, Info, Error };
    std::uint64_t id{};
    Type type{Type::Error};
    /// Action/Info: raw JSON payload (same shape as the HTTP response). Error: error text.
    std::string_view payload{};
};

/// Acknowledgement of a subscribe / unsubscribe request.
struct SubscriptionResponseMsg {
    std::string_view method{};            ///< "subscribe" / "unsubscribe"
    std::string_view subscriptionType{};  ///< "l2Book", "orderUpdates", …
    std::string_view coin{};              ///< empty for coin-less subscriptions
};

/**
 * @brief Receiver of decoded messages. Override only what you need; every
 *        method has an empty default. Dispatch is a single virtual call per message.
 */
class WsMessageHandler {
public:
    virtual ~WsMessageHandler() = default;

    virtual void onL2Book(const L2BookMsg& /*msg*/) {}
    virtual void onBbo(const BboMsg& /*msg*/) {}
    virtual void onTrades(std::span<const TradeMsg> /*trades*/) {}
    virtual void onAssetCtx(const AssetCtxMsg& /*msg*/) {}
    virtual void onAllMids(const AllMidsMsg& /*msg*/) {}
    virtual void onOrderUpdates(std::span<const OrderUpdateMsg> /*updates*/) {}
    virtual void onUserFills(const UserFillsMsg& /*msg*/) {}
    virtual void onPostResponse(const PostResponseMsg& /*msg*/) {}
    virtual void onSubscriptionResponse(const SubscriptionResponseMsg& /*msg*/) {}
    virtual void onPong() {}
    /// Venue-reported error frame (`{"channel":"error","data":"…"}`).
    virtual void onVenueError(std::string_view /*text*/) {}
    /// Any channel this parser does not decode; @p frame is the whole raw message.
    virtual void onUnhandled(std::string_view /*channel*/, std::string_view /*frame*/) {}
};

}  // namespace hl
