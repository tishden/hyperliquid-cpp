#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hl/WsMessageParser.h"
#include "hl/WsMessages.h"
#include "hl/core/Types.h"
#include "hl/md/OrderBook.h"
#include "hl/net/EventLoop.h"
#include "hl/net/WsSession.h"

namespace hl {

/// MarketDataClient configuration.
struct MarketDataConfig {
    Network network{Network::Mainnet};
    /// Overrides the network's WebSocket URL when non-empty.
    std::string urlOverride{};
    /// Connection management (url is filled from network / urlOverride).
    WsSessionOptions session{};
    /// Overlay `bbo` updates onto books maintained from `l2Book` (recommended).
    bool applyBboToBooks{true};
};

/// Optional server-side variants of an `l2Book` subscription.
struct L2BookOptions {
    std::optional<int> nSigFigs{};  ///< 2..5 — aggregate levels to N significant figures
    std::optional<int> mantissa{};  ///< 1, 2 or 5 — only with nSigFigs == 5
    /**
     * @brief `fast` — the venue's 5-level publish path instead of the 20-level one.
     *
     * Channel, frame shape and parsing are identical to a normal `l2Book`, so `book(coin)` is
     * maintained exactly as before, just five levels deep. Despite the name this is a **rate**
     * difference, not a latency one: measured on mainnet BTC and ETH on 2026-09-19, snapshots
     * arrive about every 0.54 s against about 5.3 s for the 20-level feed, while matching
     * snapshots of the two feeds by venue timestamp showed no consistent delivery lead either
     * way. Take it to keep levels 2..5 fresh; keep the default subscription when
     * `cumulativeSize` / `vwapForSize` need levels 6..20, and remember `bbo` (~7 messages per
     * second) is still the fastest source for the top of book.
     *
     * Omitted from the subscription JSON when false, so existing subscription strings are
     * unchanged byte for byte.
     */
    bool fast{false};
};

/**
 * @brief Callbacks of a MarketDataClient. Invoked on the event-loop thread.
 *
 * A callback may call any client method and may run the event loop re-entrantly; it must not
 * destroy the client or the loop, and views it receives are valid only until it returns.
 *
 * Inherits every raw message callback of WsMessageHandler (`onL2Book`, `onBbo`,
 * `onTrades`, `onAssetCtx`, `onAllMids`, …) and adds connection events plus a
 * book-level callback fired after the internal OrderBook has been updated.
 */
class MarketDataListener : public WsMessageHandler {
public:
    /// Connected and subscriptions (re)sent. Fired after every reconnect.
    virtual void onConnected() {}
    /// Connection lost; the client reconnects automatically.
    virtual void onDisconnected(std::string_view /*reason*/) {}

    /// Why a book changed.
    enum class BookUpdate : std::uint8_t { Snapshot, Bbo };
    /// The maintained book for a coin changed (after applying the message).
    virtual void onBookUpdate(const OrderBook& /*book*/, BookUpdate /*kind*/) {}
};

/**
 * @brief Public market-data feed: subscriptions, decoding and order-book maintenance.
 *
 * ```cpp
 * hl::EventLoop loop;
 * MyListener listener;
 * hl::MarketDataClient md(loop, listener, {.network = hl::Network::Testnet});
 * md.subscribeBook("BTC");      // l2Book + bbo → maintained OrderBook
 * md.subscribeTrades("BTC");
 * md.start();
 * loop.run();
 * ```
 *
 * Subscriptions may be added before or after `start()`; they are replayed on
 * reconnect. A book is kept for every coin with an `l2Book` subscription and
 * is cleared on disconnect until the next snapshot arrives.
 */
class MarketDataClient final : private WsSessionListener, private WsMessageHandler {
public:
    MarketDataClient(EventLoop& loop, MarketDataListener& listener, MarketDataConfig config = {});
    ~MarketDataClient();
    MarketDataClient(const MarketDataClient&) = delete;
    MarketDataClient& operator=(const MarketDataClient&) = delete;

    void start();
    void stop();

    /// `l2Book` — full depth snapshots; maintains `book(coin)`.
    void subscribeL2Book(std::string_view coin, L2BookOptions options = {});
    /// `bbo` — every best bid/offer change.
    void subscribeBbo(std::string_view coin);
    /// Convenience: `l2Book` + `bbo`.
    void subscribeBook(std::string_view coin, L2BookOptions options = {});
    /// `trades` — public trades (including liquidations).
    void subscribeTrades(std::string_view coin);
    /// `activeAssetCtx` — funding, mark, oracle, open interest.
    void subscribeAssetCtx(std::string_view coin);
    /// `allMids` — mids of all coins.
    void subscribeAllMids();
    /// `candle` — OHLCV bars; @p interval is "1m", "15m", "1h", "1d", …
    void subscribeCandle(std::string_view coin, std::string_view interval);
    /// `userEvents` for @p user — fills, funding, **liquidations** and venue-initiated cancels
    /// (delivered on channel `user`; see `WsMessageHandler::onLiquidation` / `onNonUserCancels`).
    void subscribeUserEvents(const Address& user);
    /// `userFundings` — funding payments of @p user.
    void subscribeUserFundings(const Address& user);
    /// `activeAssetData` — leverage and tradable size of @p user on one asset.
    void subscribeActiveAssetData(const Address& user, std::string_view coin);
    /// `notification` — venue notifications for @p user.
    void subscribeNotifications(const Address& user);
    /// Subscribe with a raw subscription object, e.g. `{"type":"candle","coin":"BTC","interval":"1m"}`.
    /// Messages of channels without a typed callback arrive in `onUnhandled`.
    void subscribeRaw(std::string subscriptionJson);
    /// Remove any subscription previously added (pass the same JSON the typed helpers generate
    /// via `subscriptionJson`).
    void unsubscribeRaw(std::string_view subscriptionJson);

    /// The subscription object a typed helper sends, e.g. `subscriptionJson("trades", "BTC")`.
    [[nodiscard]] static std::string subscriptionJson(std::string_view type, std::string_view coin = {});
    /// The subscription object `subscribeL2Book` sends — pass it to `unsubscribeRaw` to undo a
    /// subscription made with non-default options.
    [[nodiscard]] static std::string l2BookSubscriptionJson(std::string_view coin, L2BookOptions options = {});

    /// Maintained book for @p coin, or nullptr if not subscribed to `l2Book`.
    [[nodiscard]] const OrderBook* book(std::string_view coin) const noexcept;

    [[nodiscard]] bool isConnected() const noexcept { return session_.isOpen(); }
    [[nodiscard]] const WsMessageParser::Stats& parserStats() const noexcept { return parser_.stats(); }
    [[nodiscard]] std::uint64_t reconnectCount() const noexcept { return session_.reconnectCount(); }
    [[nodiscard]] WsSession& session() noexcept { return session_; }

private:
    // WsSessionListener
    void onSessionOpen() override;
    void onSessionMessage(std::string_view message) override;
    void onSessionClosed(std::string_view reason) override;

    /// The subscription object for a user channel, e.g. `{"type":"userEvents","user":"0x…"}`.
    [[nodiscard]] static std::string userSubscriptionJson(std::string_view type, const Address& user);

    // WsMessageHandler (internal: update books, then forward)
    void onL2Book(const L2BookMsg& msg) override;
    void onBbo(const BboMsg& msg) override;
    void onTrades(std::span<const TradeMsg> trades) override;
    void onAssetCtx(const AssetCtxMsg& msg) override;
    void onAllMids(const AllMidsMsg& msg) override;
    void onOrderUpdates(std::span<const OrderUpdateMsg> updates) override;
    void onUserFills(const UserFillsMsg& msg) override;
    void onCandle(const CandleMsg& msg) override;
    void onLiquidation(const LiquidationMsg& msg) override;
    void onNonUserCancels(std::span<const NonUserCancelMsg> cancels) override;
    void onUserFundings(std::span<const UserFundingMsg> fundings) override;
    void onActiveAssetData(const ActiveAssetDataMsg& msg) override;
    void onNotification(const NotificationMsg& msg) override;
    void onPostResponse(const PostResponseMsg& msg) override;
    void onSubscriptionResponse(const SubscriptionResponseMsg& msg) override;
    void onPong() override;
    void onVenueError(std::string_view text) override;
    void onUnhandled(std::string_view channel, std::string_view frame) override;

    OrderBook* findBook(std::string_view coin) noexcept;

    /// A maintained book together with the exact `l2Book` subscription string that feeds it,
    /// so `unsubscribeRaw` can drop the book on the same exact-match rule `WsSession` uses.
    struct BookEntry {
        std::unique_ptr<OrderBook> book;
        std::string subscription;
    };

    MarketDataListener& listener_;
    MarketDataConfig config_;
    WsSession session_;
    WsMessageParser parser_;
    std::vector<BookEntry> books_;
};

}  // namespace hl
