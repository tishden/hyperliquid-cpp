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

/// `{"type":"order","orders":[…],"grouping":…}` — batch of 1..N orders (one rate-limit unit per ≤ 40).
[[nodiscard]] EncodedAction order(std::span<const OrderWire> orders, std::string_view grouping = "na");
/// `{"type":"cancel","cancels":[{"a","o"},…]}`
[[nodiscard]] EncodedAction cancel(std::span<const CancelWire> cancels);
/// `{"type":"cancelByCloid","cancels":[{"asset","cloid"},…]}`
[[nodiscard]] EncodedAction cancelByCloid(std::span<const CancelByCloidWire> cancels);
/// `{"type":"batchModify","modifies":[{"oid","order"},…]}`
[[nodiscard]] EncodedAction batchModify(std::span<const ModifyWire> modifies);
/// `{"type":"scheduleCancel"[,"time":ms]}` — dead-man's switch; `nullopt` clears it.
[[nodiscard]] EncodedAction scheduleCancel(std::optional<std::uint64_t> timeMs);
/// `{"type":"updateLeverage","asset","isCross","leverage"}`
[[nodiscard]] EncodedAction updateLeverage(std::uint32_t asset, bool isCross, std::uint32_t leverage);

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
