#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hl/WsMessageParser.h"
#include "hl/core/Decimal.h"
#include "hl/core/Int128.h"
#include "hl/core/Result.h"
#include "hl/core/Types.h"
#include "hl/crypto/Signer.h"
#include "hl/net/EventLoop.h"
#include "hl/net/HttpClient.h"
#include "hl/net/WsSession.h"
#include "hl/om/Actions.h"
#include "hl/om/AssetRegistry.h"
#include "hl/om/ExchangeResponse.h"
#include "hl/om/InfoClient.h"
#include "hl/om/Types.h"

namespace hl {

/// Transport used to submit signed actions.
enum class ActionTransport : std::uint8_t {
    WebSocket,  ///< `post` over the private WebSocket (lowest latency); falls back to HTTP when disconnected
    Http,       ///< `POST /exchange`
};

/// ExchangeClient configuration.
struct ExchangeConfig {
    Network network{Network::Testnet};
    /// secp256k1 key (hex) of the signing wallet — preferably an API/agent wallet.
    std::string privateKey{};
    /// Master account address (0x…). Required when `privateKey` is an agent wallet;
    /// empty = the signer's own address.
    std::string accountAddress{};
    /// Trade on behalf of a vault or sub-account (0x…); empty = none.
    std::string vaultAddress{};
    ActionTransport transport{ActionTransport::WebSocket};
    /// Deadline for an action response before the order is reconciled via `orderStatus`.
    std::int64_t requestTimeoutMs{10'000};
    /// Also load spot assets (`spotMeta`) so spot pairs can be traded.
    bool loadSpotAssets{false};
    /// Terminal orders (filled / canceled / rejected) are kept this long for lookups, then evicted.
    std::int64_t terminalOrderRetentionMs{60'000};
    /// Overrides for proxies / mocks. Empty = network defaults.
    std::string restUrlOverride{};
    std::string wsUrlOverride{};
    /// Connection management for the private WebSocket (url is filled automatically).
    WsSessionOptions session{};
    HttpClientOptions http{};
};

/// Local lifecycle state of an order.
enum class OrderState : std::uint8_t {
    PendingNew,       ///< sent, no acknowledgement yet
    Open,             ///< resting, nothing filled
    PartiallyFilled,  ///< resting, partially filled
    Filled,           ///< terminal
    Canceled,         ///< terminal
    Rejected,         ///< terminal — see Order::lastError
};

[[nodiscard]] std::string_view toString(OrderState state) noexcept;
[[nodiscard]] constexpr bool isTerminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Canceled || s == OrderState::Rejected;
}

/// Client-side view of one order.
struct Order {
    Cloid cloid{};
    std::uint64_t oid{0};            ///< exchange id, 0 until acknowledged
    std::string coin{};
    std::uint32_t asset{};
    Side side{};
    Decimal px{};                    ///< current limit price (updated by modify)
    Decimal origSz{};                ///< current original size (updated by modify)
    Decimal filledSz{};
    Decimal avgFillPx{};
    Tif tif{Tif::Gtc};
    bool reduceOnly{false};
    bool isTrigger{false};
    OrderState state{OrderState::PendingNew};
    bool cancelPending{false};       ///< a cancel request is in flight
    bool modifyPending{false};       ///< a modify request is in flight
    bool external{false};            ///< not placed by this client (seen via orderUpdates / reconcile)
    std::string lastError{};
    std::int64_t createdMs{0};       ///< local wall clock
    std::int64_t updatedMs{0};       ///< local wall clock

    [[nodiscard]] bool isLive() const noexcept { return !isTerminal(state); }
    [[nodiscard]] Decimal remainingSz() const noexcept { return origSz - filledSz; }
};

/// A new order.
struct OrderRequest {
    std::string coin{};                   ///< "BTC", "ETH", "PURR/USDC", …
    Side side{Side::Buy};
    Decimal px{};                         ///< must satisfy AssetInfo::isValidPx (use roundPx)
    Decimal sz{};                         ///< must satisfy AssetInfo::isValidSz (use roundSz)
    Tif tif{Tif::Gtc};
    bool reduceOnly{false};
    std::optional<TriggerSpec> trigger{};
    std::optional<Cloid> cloid{};         ///< generated when empty
};

/// Callbacks of an ExchangeClient. Invoked on the event-loop thread.
class ExchangeListener {
public:
    virtual ~ExchangeListener() = default;

    /// Assets loaded and private streams subscribed — trading may start. Fired after every reconnect.
    virtual void onReady() {}
    /// Private stream lost; the client reconnects and reconciles automatically.
    virtual void onDisconnected(std::string_view /*reason*/) {}
    /// Any change of an order's state, fill quantity, price or error.
    virtual void onOrderUpdate(const Order& /*order*/) {}
    /// A new execution of one of the account's orders (deduplicated by trade id).
    virtual void onFill(const Fill& /*fill*/) {}
    /// Errors not tied to a specific order (start-up failures, venue error frames, …).
    virtual void onError(const Error& /*error*/) {}
};

/**
 * @brief Order management for one Hyperliquid account.
 *
 * Responsibilities:
 *  - start-up: load asset metadata, seed positions (`clearinghouseState`),
 *    subscribe to `orderUpdates` + `userFills`;
 *  - sign and submit order / cancel / modify / scheduleCancel / updateLeverage
 *    actions over the WebSocket (`post`) or HTTP;
 *  - maintain an order table keyed by cloid, merging acknowledgements, order
 *    updates and fills into one consistent `Order` per order;
 *  - track per-coin positions from fills;
 *  - on timeouts and reconnects, reconcile unknown orders with `orderStatus`.
 *
 * All methods must be called on the event-loop thread. Order-entry methods
 * validate synchronously and return an Error (or the cloid) immediately; the
 * venue outcome arrives via ExchangeListener::onOrderUpdate.
 */
class ExchangeClient final : private WsSessionListener, private WsMessageHandler {
public:
    /// Callback for actions without an order table effect (scheduleCancel, updateLeverage, raw).
    using ActionCallback = std::function<void(const Result<ExchangeResponse>&)>;

    /// @throws std::invalid_argument on a malformed key or address.
    ExchangeClient(EventLoop& loop, ExchangeListener& listener, ExchangeConfig config);
    ~ExchangeClient();
    ExchangeClient(const ExchangeClient&) = delete;
    ExchangeClient& operator=(const ExchangeClient&) = delete;

    void start();
    void stop();

    /// True between onReady and the next disconnect.
    [[nodiscard]] bool isReady() const noexcept { return ready_; }

    // ── order entry ─────────────────────────────────────────────────────────
    /// Place one order. Returns its cloid, or an Error{Rejected} if validation fails.
    [[nodiscard]] Result<Cloid> placeOrder(const OrderRequest& request);
    /// Place several orders in one signed action (atomic submission, one rate-limit unit per 40).
    [[nodiscard]] Result<std::vector<Cloid>> placeOrders(std::span<const OrderRequest> requests);
    /// Cancel an order placed by this client (or seen via updates).
    Error cancel(const Cloid& cloid);
    /// Cancel by exchange id (works for orders placed elsewhere).
    Error cancelByOid(std::string_view coin, std::uint64_t oid);
    /// Cancel every live order known locally, optionally restricted to one coin. One action per call.
    Error cancelAll(std::string_view coin = {});
    /// Amend price and size of a resting order (keeps the cloid; the venue assigns a new oid).
    Error modify(const Cloid& cloid, Decimal newPx, Decimal newSz);
    /// Dead-man's switch: cancel all orders at @p timeMs (≥ now + 5 s); nullopt clears it.
    Error scheduleCancel(std::optional<std::int64_t> timeMs, ActionCallback callback = {});
    /// Set leverage for a perp.
    Error updateLeverage(std::string_view coin, std::uint32_t leverage, bool isCross, ActionCallback callback = {});
    /// Sign and submit any pre-built action.
    void submitAction(const EncodedAction& action, ActionCallback callback);

    // ── state ───────────────────────────────────────────────────────────────
    [[nodiscard]] const Order* findOrder(const Cloid& cloid) const noexcept;
    /// Live orders (optionally for one coin).
    [[nodiscard]] std::vector<const Order*> liveOrders(std::string_view coin = {}) const;
    /// Signed position from `clearinghouseState` at start-up plus subsequent fills.
    [[nodiscard]] Decimal position(std::string_view coin) const noexcept;
    [[nodiscard]] const AssetRegistry& assets() const noexcept { return assets_; }
    [[nodiscard]] const Address& accountAddress() const noexcept { return account_; }
    [[nodiscard]] const Address& signerAddress() const noexcept { return signer_->address(); }
    [[nodiscard]] InfoClient& info() noexcept { return info_; }
    /// Generate a fresh, session-unique cloid.
    [[nodiscard]] Cloid nextCloid() noexcept;

    /// Counters.
    struct Stats {
        std::uint64_t actionsSent{0};
        std::uint64_t actionsViaHttp{0};
        std::uint64_t actionErrors{0};
        std::uint64_t timeouts{0};
        std::uint64_t fills{0};
        std::uint64_t reconciles{0};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    enum class ActionKind : std::uint8_t { Order, Cancel, Modify, Other };

    struct PendingAction {
        ActionKind kind{ActionKind::Other};
        std::vector<Cloid> cloids{};  ///< aligned with the action's entries; Cloid{} = untracked
        std::vector<std::pair<Decimal, Decimal>> modifyTargets{};  ///< (px, sz) per modify entry
        ActionCallback callback{};
        EventLoop::TimerId timer{0};
        bool viaWebSocket{false};
    };

    struct Tracked {
        Order order{};
        Decimal fillSum{};
        Int128 fillNotional{0};
        Decimal ackFilledSz{};
        Decimal ackAvgPx{};
        std::vector<std::uint64_t> retiredOids{};
        bool canceledDuringModify{false};
    };

    // WsSessionListener
    void onSessionOpen() override;
    void onSessionMessage(std::string_view message) override;
    void onSessionClosed(std::string_view reason) override;
    // WsMessageHandler
    void onOrderUpdates(std::span<const OrderUpdateMsg> updates) override;
    void onUserFills(const UserFillsMsg& msg) override;
    void onPostResponse(const PostResponseMsg& msg) override;
    void onSubscriptionResponse(const SubscriptionResponseMsg& msg) override;
    void onVenueError(std::string_view text) override;

    void bootstrap();
    void maybeReady();
    Error checkReady() const;
    Result<OrderWire> buildWire(const OrderRequest& request, Cloid cloid) const;
    void send(const EncodedAction& action, PendingAction pending);
    void completeAction(std::uint64_t requestId, const Result<ExchangeResponse>& result);
    void applyActionResult(const PendingAction& pending, const Result<ExchangeResponse>& result);
    void applyAck(Tracked& t, const ActionStatus& status);
    void applyVenueStatus(Tracked& t, OrderUpdateStatus status, std::string_view statusText, Decimal limitPx,
                          Decimal origSz);
    void recomputeFills(Tracked& t) noexcept;
    void setOid(Tracked& t, std::uint64_t oid);
    void reconcile(const Cloid& cloid);
    void reconcileAll();
    void emit(Tracked& t);
    Tracked* find(const Cloid& cloid) noexcept;
    Tracked* findByOid(std::uint64_t oid) noexcept;
    Tracked& track(const Cloid& cloid);
    void armEviction();

    EventLoop& loop_;
    ExchangeListener& listener_;
    ExchangeConfig config_;
    std::unique_ptr<Signer> signer_;
    std::optional<Address> vault_;
    Address account_{};
    Address user_{};  ///< vault if set, else account — owner of orders / fills
    RequestBuilder builder_;
    NonceGenerator nonces_;
    InfoClient info_;
    HttpClient exchangeHttp_;
    WsSession session_;
    WsMessageParser parser_;
    AssetRegistry assets_;

    std::unordered_map<Cloid, Tracked, CloidHash> orders_;
    std::unordered_map<std::uint64_t, Cloid> oidIndex_;
    std::unordered_map<std::string, Decimal> positions_;
    std::unordered_set<std::uint64_t> seenTids_;
    std::deque<std::uint64_t> seenTidOrder_;
    std::unordered_map<std::uint64_t, PendingAction> pending_;
    std::uint64_t nextRequestId_{1};
    std::uint64_t cloidSession_{0};
    std::uint64_t cloidCounter_{0};

    bool started_{false};
    bool assetsLoaded_{false};
    bool positionsLoaded_{false};
    bool orderUpdatesAcked_{false};
    bool userFillsAcked_{false};
    bool ready_{false};
    bool wasReadyBefore_{false};
    bool fillsSnapshotSeen_{false};
    EventLoop::TimerId evictionTimer_{0};
    Stats stats_{};
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};  ///< guards deferred retries
};

}  // namespace hl
