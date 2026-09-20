// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <chrono>
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
    /**
     * Precomputed-nonce ECDSA pool size (0 = off). When > 0 the Signer keeps this many nonces ready
     * on an internal background thread, cutting signing from ~15 µs to ~0.05 µs per action
     * (see Signer::enableNoncePool). Signatures are randomised instead of RFC 6979.
     */
    std::size_t precomputedNonces{0};
    /// Deadline for an action response before the order is reconciled via `orderStatus`.
    std::int64_t requestTimeoutMs{10'000};
    /// Also load spot assets (`spotMeta`) so spot pairs can be traded, and seed spot balances.
    bool loadSpotAssets{false};
    /// Subscribe to `userEvents` as well, which is the only source of liquidation notifications
    /// and of cancels performed by the venue itself.
    bool subscribeUserEvents{true};
    /// At start-up, adopt orders the venue already has open for this account (as `Order::external`),
    /// so a restarted process can see and cancel what the previous one left behind.
    bool adoptExistingOrders{true};
    /// Builder code applied to order actions that do not pass one explicitly (requires `approveBuilderFee`).
    std::optional<BuilderFee> builderFee{};
    /// Send cancels with the venue's `fast` flag (future mempool prioritisation). Trigger orders are
    /// always canceled without it, because the venue rejects fast cancels for them.
    bool fastCancels{true};
    /**
     * Attach `expiresAfter = now + actionExpiryMs` to every signed action (0 = off). The venue drops
     * an action that reaches it later than this — a cheap per-action dead-man's switch for stale
     * orders after a network stall. Note: actions rejected for a stale `expiresAfter` cost 5× the
     * usual address-based rate-limit budget.
     */
    std::int64_t actionExpiryMs{0};
    /**
     * How often to refresh the account's request budget from `userRateLimit` (0 = never).
     * Hyperliquid grants ~1 request per USDC of traded volume on top of a 10 000 buffer, counted
     * **per order or cancel in a batch**; running out throttles the account to one request per
     * 10 s, so a market maker should watch it.
     */
    std::int64_t rateLimitRefreshMs{60'000};
    /// Warn (once per crossing) when the remaining request budget falls below this fraction.
    double rateLimitWarnFraction{0.1};
    /**
     * Nonce source. The venue tracks nonces **per signing key**, so two clients that share a key
     * (e.g. one for the master account and one for a sub-account) must share one generator.
     * Empty = this client owns its own.
     */
    std::shared_ptr<NonceGenerator> nonces{};
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
    std::optional<TriggerSpec> trigger{};  ///< set for stop / take-profit orders; preserved by modify()
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

/**
 * @brief Callbacks of an ExchangeClient. Invoked on the event-loop thread.
 *
 * What a callback may do: call any client method (place, cancel, modify, queries), and run the
 * event loop re-entrantly (the parsers and transports are re-entrancy safe, at the cost of extra
 * buffers). What it must not do: destroy the client or the loop it is called from, or block for
 * long — every other client on the loop is stalled meanwhile.
 */
class ExchangeListener {
public:
    virtual ~ExchangeListener() = default;

    /// Assets loaded and private streams subscribed — trading may start. Fired after every reconnect.
    virtual void onReady() {}
    /// Private stream lost; the client reconnects and reconciles automatically.
    virtual void onDisconnected(std::string_view /*reason*/) {}
    /// Any change of an order's state, fill quantity, price, pending flags or error — once per
    /// change: one venue event can reach the client as a stream update, an acknowledgement and a
    /// reconciliation answer, and an update that changes nothing observable is not delivered.
    virtual void onOrderUpdate(const Order& /*order*/) {}
    /// A new execution of one of the account's orders (deduplicated by trade id).
    virtual void onFill(const Fill& /*fill*/) {}
    /// This account was liquidated — positions were closed by the venue (requires `subscribeUserEvents`).
    virtual void onLiquidation(const LiquidationMsg& /*msg*/) {}
    /// A funding payment was applied to the account (requires `subscribeUserEvents`).
    virtual void onFunding(const UserFundingMsg& /*msg*/) {}
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
    /**
     * @brief Place several orders in one signed action (atomic submission, one rate-limit unit per 40).
     * @param grouping `Grouping::NormalTpsl` / `PositionTpsl` attach the trigger orders that follow the
     *                 parent order in @p requests as its take-profit / stop-loss children; a
     *                 `PriorityRate` instead pays an order-priority fee for the whole action.
     * @param builder  Builder code for this action; defaults to `ExchangeConfig::builderFee`.
     */
    [[nodiscard]] Result<std::vector<Cloid>> placeOrders(std::span<const OrderRequest> requests,
                                                         OrderGrouping grouping = Grouping::Na,
                                                         const std::optional<BuilderFee>& builder = std::nullopt);
    /// Cancel an order placed by this client (or seen via updates).
    Error cancel(const Cloid& cloid);
    /// Cancel by exchange id (works for orders placed elsewhere).
    Error cancelByOid(std::string_view coin, std::uint64_t oid);
    /// Cancel every live order known locally, optionally restricted to one coin. One action per call.
    Error cancelAll(std::string_view coin = {});
    /**
     * @brief Amend a resting order in place: same cloid, side, time-in-force and reduce-only flag.
     *
     * The venue implements this as cancel + replace, so the exchange order id usually changes; the
     * client follows it (see docs/ORDER_MANAGEMENT.md §9). A trigger order keeps its trigger unless
     * @p newTrigger is given — @p newPx is the limit price, not the trigger price.
     */
    Error modify(const Cloid& cloid, Decimal newPx, Decimal newSz,
                 std::optional<TriggerSpec> newTrigger = std::nullopt);
    /**
     * @brief Dead-man's switch: ask the venue to cancel all orders at @p timeMs; nullopt clears it.
     *
     * The time must be at least 5 s in the future (validated locally). The venue only enables the
     * feature for accounts with at least $1 000 000 of traded volume and allows at most 10 triggers
     * per UTC day — otherwise it answers with `Error{Venue}` explaining which limit was hit.
     */
    Error scheduleCancel(std::optional<std::int64_t> timeMs, ActionCallback callback = {});
    /// Set leverage for a perp.
    Error updateLeverage(std::string_view coin, std::uint32_t leverage, bool isCross, ActionCallback callback = {});
    /// Add (positive) or remove (negative) isolated margin on a perp position, in USDC (≥ 1e-6 granularity).
    Error updateIsolatedMargin(std::string_view coin, Decimal usdc, ActionCallback callback = {});
    /// Spend accumulated rate-limit budget to reserve additional request weight.
    Error reserveRequestWeight(std::uint64_t weight, ActionCallback callback = {});
    /// Send a `noop` action: no market effect, burns one nonce (signing health check / nonce advance).
    Error noop(ActionCallback callback = {});
    /// Sign and submit any pre-built action.
    void submitAction(const EncodedAction& action, ActionCallback callback);

    /**
     * @brief Send an `/info` request over the private WebSocket instead of HTTP.
     *
     * Saves an HTTP round trip on the hot path (e.g. `orderStatus` during reconciliation). The raw
     * JSON payload of the response is handed to @p callback. Falls back to `Error{Transport}` when
     * the socket is not open; the request is not queued.
     */
    void infoOverWebSocket(std::string infoJson, std::function<void(const Result<std::string>&)> callback);

    // ── state ───────────────────────────────────────────────────────────────
    [[nodiscard]] const Order* findOrder(const Cloid& cloid) const noexcept;
    /// Live orders (optionally for one coin).
    [[nodiscard]] std::vector<const Order*> liveOrders(std::string_view coin = {}) const;
    /// Signed position from `clearinghouseState` at start-up plus subsequent fills.
    [[nodiscard]] Decimal position(std::string_view coin) const noexcept;
    /**
     * @brief Spot balance of one token ("USDC", "HYPE"), or zero if it is unknown.
     *
     * Only populated when `ExchangeConfig::loadSpotAssets` is set. On a unified account — the
     * venue's default — the USDC balance here is the collateral behind both spot and perp
     * trading, while `AccountState::accountValue` counts only what is committed to perp
     * positions. This is the figure to size against.
     */
    [[nodiscard]] Decimal spotTokenBalance(std::string_view token) const noexcept;
    [[nodiscard]] const AssetRegistry& assets() const noexcept { return assets_; }
    [[nodiscard]] const Address& accountAddress() const noexcept { return account_; }
    [[nodiscard]] const Address& signerAddress() const noexcept { return signer_->address(); }
    [[nodiscard]] InfoClient& info() noexcept { return info_; }
    /// The private WebSocket session (subscriptions, reconnect counter, `reconnectNow`).
    [[nodiscard]] WsSession& session() noexcept { return session_; }
    /// Generate a fresh, session-unique cloid.
    [[nodiscard]] Cloid nextCloid() noexcept;

    /// Counters.
    /**
     * @brief Latency of one class of action, in microseconds.
     *
     * Measured with a steady clock, so it is unaffected by wall-clock adjustments. The round-trip
     * figures span the moment the client starts building the action to the moment the venue's
     * response for it is parsed, and therefore include network time to the venue and back.
     */
    struct LatencyStats {
        std::uint64_t count{0};
        std::uint64_t sumUs{0};
        std::uint32_t minUs{0};
        std::uint32_t maxUs{0};
        std::uint32_t lastUs{0};
        [[nodiscard]] std::uint32_t meanUs() const noexcept {
            return count == 0 ? 0 : static_cast<std::uint32_t>(sumUs / count);
        }
        void add(std::uint32_t us) noexcept {
            minUs = (count == 0 || us < minUs) ? us : minUs;
            maxUs = us > maxUs ? us : maxUs;
            lastUs = us;
            sumUs += us;
            ++count;
        }
    };

    struct Stats {
        std::uint64_t actionsSent{0};
        std::uint64_t actionsViaHttp{0};
        std::uint64_t actionErrors{0};
        std::uint64_t timeouts{0};
        std::uint64_t fills{0};
        std::uint64_t reconciles{0};
        std::uint64_t rateLimitHits{0};    ///< HTTP 429 responses (the queue pauses after each)
        std::uint64_t addressUnitsUsed{0}; ///< order/cancel entries submitted (the venue's address-based unit)
        /// Local work only: build the action, hash it, sign it and assemble the frame.
        LatencyStats buildAndSign{};
        /// Build+sign+network+venue, per action class. Cancels are the cheapest, orders the most
        /// interesting; `other` covers leverage, scheduleCancel and the rest.
        LatencyStats orderRoundTrip{};
        LatencyStats cancelRoundTrip{};
        LatencyStats modifyRoundTrip{};
        LatencyStats otherRoundTrip{};
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    /**
     * @brief Latest known request budget of the account.
     *
     * `requestsUsed` is the venue's figure from the last `userRateLimit` refresh plus the entries
     * this client submitted since; `requestsCap` comes from the venue (0 = not fetched yet).
     */
    [[nodiscard]] RateLimitStatus rateLimitStatus() const noexcept;
    /// Signature counters by path (precomputed nonce vs deterministic fallback).
    [[nodiscard]] Signer::SigningStats signingStats() const noexcept { return signer_->signingStats(); }

private:
    enum class ActionKind : std::uint8_t { Order, Cancel, Modify, Other };

    struct PendingAction {
        ActionKind kind{ActionKind::Other};
        std::vector<Cloid> cloids{};  ///< aligned with the action's entries; Cloid{} = untracked
        std::vector<std::pair<Decimal, Decimal>> modifyTargets{};  ///< (px, sz) per modify entry
        std::optional<TriggerSpec> modifyTrigger{};                ///< new trigger, when the caller changed it
        ActionCallback callback{};
        EventLoop::TimerId timer{0};
        bool viaWebSocket{false};
        std::chrono::steady_clock::time_point sentAt{};
    };

    struct Tracked {
        Order order{};
        Decimal fillSum{};
        Int128 fillNotional{0};
        Decimal ackFilledSz{};
        Decimal ackAvgPx{};
        std::vector<std::uint64_t> retiredOids{};  ///< bounded: only the most recent amendments matter
        bool canceledDuringModify{false};
        int notFoundProbes{0};  ///< how often the venue has answered "unknown" about an unacked order

        /// What the listener was last told, so the same news is not delivered twice: one venue
        /// rejection can arrive as an `orderUpdates` message, as an action acknowledgement and as a
        /// reconciliation answer, and `onOrderUpdate` promises *changes*.
        struct Emitted {
            OrderState state{OrderState::PendingNew};
            std::uint64_t oid{0};
            Decimal px{};
            Decimal origSz{};
            Decimal filledSz{};
            Decimal avgFillPx{};
            bool cancelPending{false};
            bool modifyPending{false};
            bool everEmitted{false};
            std::string lastError{};
            bool operator==(const Emitted&) const = default;
        };
        Emitted emitted{};

        void retireOid(std::uint64_t oid) {
            retiredOids.push_back(oid);
            if (retiredOids.size() > 4) {
                retiredOids.erase(retiredOids.begin());
            }
        }
    };

    // WsSessionListener
    void onSessionOpen() override;
    void onSessionMessage(std::string_view message) override;
    void onSessionClosed(std::string_view reason) override;
    // WsMessageHandler
    void onOrderUpdates(std::span<const OrderUpdateMsg> updates) override;
    void onUserFills(const UserFillsMsg& msg) override;
    void onPostResponse(const PostResponseMsg& msg) override;
    void onLiquidation(const LiquidationMsg& msg) override;
    void onNonUserCancels(std::span<const NonUserCancelMsg> cancels) override;
    void onUserFundings(std::span<const UserFundingMsg> fundings) override;
    void onSubscriptionResponse(const SubscriptionResponseMsg& msg) override;
    void onVenueError(std::string_view text) override;

    void bootstrap();
    void adoptOpenOrders();
    std::size_t adoptFromListing(const std::vector<OpenOrder>& openOrders);
    void seedFilled(Tracked& t, const OpenOrder& open);
    void resubscribeUser();
    void maybeReady();
    /// Give each spot market the balance of its base token; needs spotMeta and the balances both in.
    void seedSpotPositions();
    /// One start-up line describing where the money is; see spotTokenBalance().
    void logAccountSummary() const;
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
    void reconcileAmended(const Cloid& cloid);
    void reconcileAll();
    void emit(Tracked& t);
    Tracked* find(const Cloid& cloid) noexcept;
    Tracked* findByOid(std::uint64_t oid) noexcept;
    Tracked& track(const Cloid& cloid);
    void armEviction();
    void armRateLimitRefresh();
    void countRateLimitUnits(const PendingAction& pending);

    EventLoop& loop_;
    ExchangeListener& listener_;
    ExchangeConfig config_;
    std::unique_ptr<Signer> signer_;
    std::optional<Address> vault_;
    Address account_{};
    Address user_{};  ///< vault if set, else account — owner of orders / fills
    RequestBuilder builder_;
    std::shared_ptr<NonceGenerator> nonces_;
    InfoClient info_;
    HttpClient exchangeHttp_;
    WsSession session_;
    WsMessageParser parser_;
    AssetRegistry assets_;

    std::unordered_map<Cloid, Tracked, CloidHash> orders_;
    std::unordered_map<std::uint64_t, Cloid> oidIndex_;
    struct PositionState {
        Decimal size{};
        std::int64_t lastFillMs{0};  ///< venue time of the newest fill applied to this coin
    };
    std::unordered_map<std::string, PositionState> positions_;
    std::unordered_map<std::string, Decimal> spotTokens_;  ///< token → balance (`loadSpotAssets`)
    Decimal accountValue_{};                               ///< last `marginSummary.accountValue`
    std::unordered_set<std::uint64_t> seenTids_;
    std::deque<std::uint64_t> seenTidOrder_;
    std::unordered_map<std::uint64_t, PendingAction> pending_;
    /// WebSocket `info` requests awaiting a response, keyed by request id.
    std::unordered_map<std::uint64_t, std::function<void(const Result<std::string>&)>> pendingInfo_;
    std::uint64_t nextRequestId_{1};
    std::uint64_t cloidSession_{0};
    std::uint64_t cloidCounter_{0};

    bool started_{false};
    bool assetsLoaded_{false};
    bool positionsLoaded_{false};
    bool spotBalancesLoaded_{false};
    bool orderUpdatesAcked_{false};
    bool userFillsAcked_{false};
    bool ready_{false};
    bool wasReadyBefore_{false};
    /// Set once the start-up `openOrders` listing has been applied (or has failed). Readiness
    /// waits for it, so `liveOrders()` and `cancelAll()` are correct from the first `onReady()`.
    bool ordersAdopted_{false};
    bool adoptionInFlight_{false};
    bool fillsSnapshotSeen_{false};
    EventLoop::TimerId evictionTimer_{0};
    EventLoop::TimerId rateLimitTimer_{0};
    RateLimitStatus rateLimit_{};
    std::uint64_t unitsAtLastRefresh_{0};
    bool rateLimitWarned_{false};
    Stats stats_{};
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};  ///< guards deferred retries
};

}  // namespace hl
