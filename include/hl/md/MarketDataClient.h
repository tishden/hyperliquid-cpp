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

/// Optional server-side aggregation of an `l2Book` subscription.
struct L2BookOptions {
    std::optional<int> nSigFigs{};  ///< 2..5 — aggregate levels to N significant figures
    std::optional<int> mantissa{};  ///< 1, 2 or 5 — only with nSigFigs == 5
};

/**
 * @brief Callbacks of a MarketDataClient.
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
    void subscribeBook(std::string_view coin);
    /// `trades` — public trades (including liquidations).
    void subscribeTrades(std::string_view coin);
    /// `activeAssetCtx` — funding, mark, oracle, open interest.
    void subscribeAssetCtx(std::string_view coin);
    /// `allMids` — mids of all coins.
    void subscribeAllMids();
    /// Subscribe with a raw subscription object, e.g. `{"type":"candle","coin":"BTC","interval":"1m"}`.
    /// Messages of channels without a typed callback arrive in `onUnhandled`.
    void subscribeRaw(std::string subscriptionJson);
    /// Remove any subscription previously added (pass the same JSON the typed helpers generate
    /// via `subscriptionJson`).
    void unsubscribeRaw(std::string_view subscriptionJson);

    /// The subscription object a typed helper sends, e.g. `subscriptionJson("trades", "BTC")`.
    [[nodiscard]] static std::string subscriptionJson(std::string_view type, std::string_view coin = {});

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

    // WsMessageHandler (internal: update books, then forward)
    void onL2Book(const L2BookMsg& msg) override;
    void onBbo(const BboMsg& msg) override;
    void onTrades(std::span<const TradeMsg> trades) override;
    void onAssetCtx(const AssetCtxMsg& msg) override;
    void onAllMids(const AllMidsMsg& msg) override;
    void onOrderUpdates(std::span<const OrderUpdateMsg> updates) override;
    void onUserFills(const UserFillsMsg& msg) override;
    void onPostResponse(const PostResponseMsg& msg) override;
    void onSubscriptionResponse(const SubscriptionResponseMsg& msg) override;
    void onPong() override;
    void onVenueError(std::string_view text) override;
    void onUnhandled(std::string_view channel, std::string_view frame) override;

    OrderBook* findBook(std::string_view coin) noexcept;

    MarketDataListener& listener_;
    MarketDataConfig config_;
    WsSession session_;
    WsMessageParser parser_;
    std::vector<std::unique_ptr<OrderBook>> books_;
};

}  // namespace hl
