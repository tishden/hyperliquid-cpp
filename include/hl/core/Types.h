// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hl {

/// Which Hyperliquid deployment to talk to.
enum class Network : std::uint8_t {
    Mainnet,  ///< api.hyperliquid.xyz
    Testnet,  ///< api.hyperliquid-testnet.xyz
};

/// REST base URL for a network (`https://api.hyperliquid.xyz` / `https://api.hyperliquid-testnet.xyz`).
[[nodiscard]] std::string_view restUrl(Network network) noexcept;
/// WebSocket URL for a network (`wss://api.hyperliquid.xyz/ws` / `wss://api.hyperliquid-testnet.xyz/ws`).
[[nodiscard]] std::string_view wsUrl(Network network) noexcept;

/// Order / trade side. For trades this is the *aggressor* side (HL "B" = buy, "A" = sell).
enum class Side : std::uint8_t {
    Buy,
    Sell,
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }
[[nodiscard]] std::string_view toString(Side side) noexcept;

/// Time-in-force of a limit order.
enum class Tif : std::uint8_t {
    Alo,  ///< Add-liquidity-only (post-only): rejected if it would cross.
    Ioc,  ///< Immediate-or-cancel: unfilled remainder is canceled.
    Gtc,  ///< Good-till-canceled: rests on the book.
};

[[nodiscard]] std::string_view toString(Tif tif) noexcept;

/**
 * @brief True when @p coin only contains characters Hyperliquid uses in market names
 *        (letters, digits, `/`, `@`, `-`, `_`, `.`, `:`).
 *
 * Market names are interpolated into JSON requests and subscriptions; validating them keeps a
 * caller-supplied string from producing malformed JSON.
 */
[[nodiscard]] bool isValidCoinName(std::string_view coin) noexcept;

/// 20-byte EVM address.
using Address = std::array<std::uint8_t, 20>;

/// Parse a `0x`-prefixed (or bare) 40-hex-digit address. Case-insensitive.
[[nodiscard]] std::optional<Address> parseAddress(std::string_view hex) noexcept;
/// Lower-case `0x`-prefixed hex form of an address (the form HL expects in requests).
[[nodiscard]] std::string toHex(const Address& address);

/**
 * @brief 128-bit client order id (HL "cloid"), wire form `0x` + 32 lower-case hex digits.
 *
 * Chosen by the client, unique per account, and echoed back in every order
 * update and fill — it is the stable key for an order across its lifetime and
 * across amendments.
 */
class Cloid {
public:
    constexpr Cloid() noexcept = default;
    constexpr Cloid(std::uint64_t high, std::uint64_t low) noexcept : high_(high), low_(low) {}

    /// Construct from a 64-bit id (high half zero) — the common case for sequential ids.
    [[nodiscard]] static constexpr Cloid fromU64(std::uint64_t id) noexcept { return Cloid{0, id}; }

    /// Parse `0x` + 32 hex digits. Returns nullopt on malformed input.
    [[nodiscard]] static std::optional<Cloid> parse(std::string_view hex) noexcept;

    [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
    [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }

    /// Wire form: `0x` + 32 lower-case hex digits.
    [[nodiscard]] std::string toString() const;

    constexpr auto operator<=>(const Cloid&) const noexcept = default;
    constexpr bool operator==(const Cloid&) const noexcept = default;

private:
    std::uint64_t high_{0};
    std::uint64_t low_{0};
};

/// Hash functor so Cloid can key unordered containers.
struct CloidHash {
    std::size_t operator()(const Cloid& c) const noexcept {
        return static_cast<std::size_t>(c.low() * 0x9E3779B97F4A7C15ULL ^ c.high());
    }
};

/// Error delivered to asynchronous callbacks.
struct Error {
    enum class Kind : std::uint8_t {
        None,
        Transport,  ///< TCP/TLS/WebSocket failure or disconnect
        Timeout,    ///< no response within the configured deadline
        Http,       ///< non-2xx HTTP status
        Venue,      ///< HL returned `status: err` or a per-order error
        Parse,      ///< malformed / unexpected response
        Rejected,   ///< refused locally before sending (validation)
    };

    Kind kind{Kind::None};
    int httpStatus{0};
    std::string message{};

    [[nodiscard]] explicit operator bool() const noexcept { return kind != Kind::None; }
};

[[nodiscard]] std::string_view toString(Error::Kind kind) noexcept;

}  // namespace hl
