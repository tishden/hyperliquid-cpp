#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "hl/core/Decimal.h"
#include "hl/core/Result.h"
#include "hl/core/Types.h"
#include "hl/crypto/Signer.h"

namespace hl {

/// Trigger (stop / take-profit) parameters for a conditional order.
struct TriggerSpec {
    enum class Kind : std::uint8_t { TakeProfit, StopLoss };

    Decimal triggerPx{};   ///< mark price that arms the order
    bool isMarket{true};   ///< execute as market once triggered (else as limit at `OrderWire::px`)
    Kind kind{Kind::StopLoss};
};

/**
 * @brief One order in venue form — already resolved to an asset index and
 *        rounded to valid price / size increments (see AssetInfo).
 */
struct OrderWire {
    std::uint32_t asset{};               ///< perp index, or 10000 + spot index
    bool isBuy{};
    Decimal px{};                        ///< limit price (worst price for trigger-market orders)
    Decimal sz{};                        ///< size in base units
    bool reduceOnly{false};
    Tif tif{Tif::Gtc};                   ///< ignored when `trigger` is set
    std::optional<TriggerSpec> trigger{};
    std::optional<Cloid> cloid{};
};

/**
 * @brief How the venue links the orders of one `order` action.
 *
 * `NormalTpsl` and `PositionTpsl` attach take-profit / stop-loss children to a parent order or to the
 * whole position: send the parent first and the trigger orders after it **in the same action**.
 */
enum class Grouping : std::uint8_t {
    Na,           ///< independent orders (the default)
    NormalTpsl,   ///< parent order + its TP/SL children
    PositionTpsl, ///< TP/SL attached to the position rather than to one order
};

[[nodiscard]] std::string_view groupingWire(Grouping grouping) noexcept;

/**
 * @brief Order-priority fee (`grouping: {"p": rate}`), the venue's alternative to co-location.
 *
 * `rate` is a fraction of 1e8 of the filled notional (IOC) or resting notional (ALO), charged from
 * the undelegated staking balance. The venue only accepts it when every order in the action is IOC,
 * or every order is a non-reduce-only ALO, and no order is on an outcome asset.
 * Empirically ~45 ms of end-to-end latency per basis point (`rate = 10000`) up to 8 bps.
 */
struct PriorityRate {
    std::uint32_t rate{};  ///< fraction of 1e8, e.g. 10000 = 1 bp
};

/// Either a named grouping or a priority-fee rate.
using OrderGrouping = std::variant<Grouping, PriorityRate>;

/**
 * @brief Builder-code fee attached to an order action.
 *
 * Routes a share of the trading fee to a builder address that the account has approved
 * (`approveBuilderFee`, done once outside this library).
 */
struct BuilderFee {
    Address address{};              ///< builder address (sent lower-case)
    std::uint32_t feeTenthsOfBps{}; ///< fee in tenths of a basis point, e.g. 10 = 1 bp
};

/// Cancel by exchange order id.
struct CancelWire {
    std::uint32_t asset{};
    std::uint64_t oid{};
};

/// Cancel by client order id.
struct CancelByCloidWire {
    std::uint32_t asset{};
    Cloid cloid{};
};

/// Amend an order in place (identified by exchange oid or cloid).
struct ModifyWire {
    std::variant<std::uint64_t, Cloid> target{};
    OrderWire order{};
};

/**
 * @brief An L1 action in its two required encodings.
 *
 * `msgpack` is what gets hashed and signed; `json` is what travels on the wire.
 * Both are produced from the same inputs by the builders in namespace `hl::actions`.
 */
struct EncodedAction {
    std::vector<std::uint8_t> msgpack;
    std::string json;
};

namespace actions {

/// `{"type":"order","orders":[…],"grouping":…[,"builder":{…}]}` — batch of 1..N orders (one rate-limit unit per ≤ 40).
[[nodiscard]] EncodedAction order(std::span<const OrderWire> orders, OrderGrouping grouping = Grouping::Na,
                                  const std::optional<BuilderFee>& builder = std::nullopt);
/**
 * @brief `{"type":"cancel","cancels":[{"a","o"},…][,"f":true]}`
 * @param fast sets the venue's `fast` flag (future mempool prioritisation of cancels). The venue
 *             rejects fast cancels of **trigger** orders, so it is omitted when false.
 */
[[nodiscard]] EncodedAction cancel(std::span<const CancelWire> cancels, bool fast = false);
/// `{"type":"cancelByCloid","cancels":[{"asset","cloid"},…][,"f":true]}` — see cancel() for @p fast.
[[nodiscard]] EncodedAction cancelByCloid(std::span<const CancelByCloidWire> cancels, bool fast = false);
/// `{"type":"batchModify","modifies":[{"oid","order"},…]}`
[[nodiscard]] EncodedAction batchModify(std::span<const ModifyWire> modifies);
/// `{"type":"scheduleCancel"[,"time":ms]}` — dead-man's switch; `nullopt` clears it.
[[nodiscard]] EncodedAction scheduleCancel(std::optional<std::uint64_t> timeMs);
/// `{"type":"updateLeverage","asset","isCross","leverage"}`
[[nodiscard]] EncodedAction updateLeverage(std::uint32_t asset, bool isCross, std::uint32_t leverage);
/**
 * @brief `{"type":"updateIsolatedMargin","asset","isBuy":true,"ntli"}` — add or remove isolated margin.
 * @param usdc Amount to add (positive) or remove (negative). Must be a whole number of micro-USDC
 *             (six decimals); finer amounts are rejected with `Error{Rejected}`.
 */
[[nodiscard]] Result<EncodedAction> updateIsolatedMargin(std::uint32_t asset, Decimal usdc);
/// `{"type":"noop"}` — does nothing; burns a nonce (useful to test signing or to advance the nonce window).
[[nodiscard]] EncodedAction noop();
/// `{"type":"reserveRequestWeight","weight"}` — spend accumulated rate-limit budget to reserve request weight.
[[nodiscard]] EncodedAction reserveRequestWeight(std::uint64_t weight);

}  // namespace actions

/**
 * @brief Strictly increasing millisecond nonce source.
 *
 * HL requires every signed action to carry a nonce that is unique among the
 * signer's 100 most recent nonces and within (now − 2 days, now + 1 day).
 * `next()` returns `max(wallClockMs, last + 1)`. Lock-free and thread-safe.
 */
class NonceGenerator {
public:
    [[nodiscard]] std::uint64_t next() noexcept;

private:
    std::atomic<std::uint64_t> last_{0};
};

/**
 * @brief Signs actions and wraps them into the `/exchange` payload
 *        `{"action","nonce","signature","vaultAddress"[,"expiresAfter"]}`.
 */
class RequestBuilder {
public:
    /**
     * @param signer        key used to sign (outlives the builder)
     * @param network       selects the EIP-712 `source` ("a" mainnet / "b" testnet)
     * @param vault         optional vault / sub-account address the action is for
     */
    RequestBuilder(const Signer& signer, Network network, std::optional<Address> vault = std::nullopt) noexcept
        : signer_(signer), isMainnet_(network == Network::Mainnet), vault_(vault) {}

    /// Signed `/exchange` JSON body for an action.
    [[nodiscard]] std::string payload(const EncodedAction& action, std::uint64_t nonce,
                                      std::optional<std::uint64_t> expiresAfter = std::nullopt) const;

    /// Append the signed payload to @p out (no temporary string).
    void appendPayload(std::string& out, const EncodedAction& action, std::uint64_t nonce,
                       std::optional<std::uint64_t> expiresAfter = std::nullopt) const;

    /**
     * @brief Sign and build the complete WebSocket post frame in one allocation — equivalent to
     *        `wsPost(requestId, payload(action, nonce, expiresAfter))`. This is the order-entry fast path.
     */
    [[nodiscard]] std::string wsPostAction(std::uint64_t requestId, const EncodedAction& action, std::uint64_t nonce,
                                           std::optional<std::uint64_t> expiresAfter = std::nullopt) const;

    /// WebSocket post frame `{"method":"post","id":…,"request":{"type":"action","payload":…}}`.
    [[nodiscard]] static std::string wsPost(std::uint64_t requestId, std::string_view payloadJson);
    /// WebSocket info-request frame `{"method":"post","id":…,"request":{"type":"info","payload":…}}`.
    [[nodiscard]] static std::string wsInfo(std::uint64_t requestId, std::string_view infoJson);

    [[nodiscard]] bool isMainnet() const noexcept { return isMainnet_; }
    [[nodiscard]] const Signer& signer() const noexcept { return signer_; }

private:
    const Signer& signer_;
    bool isMainnet_;
    std::optional<Address> vault_;
};

}  // namespace hl
