// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <compare>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace hl {

/**
 * @brief Exact fixed-point decimal with 8 fractional digits (value = raw / 10^8).
 *
 * Hyperliquid transmits every price and size as a decimal *string* and requires
 * order prices/sizes to be sent as canonical decimal strings, so floating point
 * is never used on the wire path. Eight fractional digits is the maximum
 * precision HL accepts on any market (spot prices), and an int64 mantissa at
 * this scale covers ±92 233 720 368.54775807 — ample for any price, size or
 * notional on the venue.
 *
 * Parsing is exact and allocation-free; digits beyond the 8th fractional place
 * are truncated (they only occur in informational fields such as `dayNtlVlm`).
 * All arithmetic is plain integer arithmetic on the mantissa.
 */
class Decimal {
public:
    static constexpr int kDecimals = 8;
    static constexpr std::int64_t kScale = 100'000'000;

    constexpr Decimal() noexcept = default;

    /// Construct from a raw mantissa (value = raw / 10^8).
    [[nodiscard]] static constexpr Decimal fromRaw(std::int64_t raw) noexcept { return Decimal{raw}; }

    /// Construct from an integer number of units (e.g. `Decimal::fromInt(5)` == 5.0).
    [[nodiscard]] static constexpr Decimal fromInt(std::int64_t units) noexcept { return Decimal{units * kScale}; }

    /// Convert from a double, rounding half away from zero to 8 decimals. Not for wire use.
    [[nodiscard]] static Decimal fromDouble(double value) noexcept;

    /**
     * @brief Parse a decimal string such as `"75951.0"`, `"-0.0001711314"`, `"12"`.
     * @param text   Input without surrounding whitespace or exponent.
     * @param out    Receives the value on success; untouched on failure.
     * @return false on empty input, stray characters or int64 overflow.
     */
    [[nodiscard]] static bool parse(std::string_view text, Decimal& out) noexcept;

    /// Parse or return zero on malformed input (convenience for trusted venue data).
    [[nodiscard]] static Decimal parseOrZero(std::string_view text) noexcept {
        Decimal d;
        return parse(text, d) ? d : Decimal{};
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    [[nodiscard]] double toDouble() const noexcept { return static_cast<double>(raw_) / static_cast<double>(kScale); }
    [[nodiscard]] constexpr bool isZero() const noexcept { return raw_ == 0; }
    [[nodiscard]] constexpr bool isNegative() const noexcept { return raw_ < 0; }

    /**
     * @brief Canonical HL wire form: shortest representation, no trailing zeros,
     *        no trailing dot, `"0"` for zero (matches the reference SDK's `float_to_wire`).
     */
    [[nodiscard]] std::string toString() const;

    /// Append the wire form to @p out without allocating a temporary.
    void appendTo(std::string& out) const;

    /// Maximum length of the wire form (sign + 11 integer digits + '.' + 8 decimals).
    static constexpr std::size_t kMaxChars = 24;
    /// Write the wire form into @p out (at least kMaxChars bytes, not NUL-terminated); returns its length.
    std::size_t toChars(char* out) const noexcept;

    constexpr Decimal operator-() const noexcept { return Decimal{-raw_}; }
    constexpr Decimal operator+(Decimal o) const noexcept { return Decimal{raw_ + o.raw_}; }
    constexpr Decimal operator-(Decimal o) const noexcept { return Decimal{raw_ - o.raw_}; }
    constexpr Decimal& operator+=(Decimal o) noexcept { raw_ += o.raw_; return *this; }
    constexpr Decimal& operator-=(Decimal o) noexcept { raw_ -= o.raw_; return *this; }

    /**
     * @brief Product of two decimals, truncated toward zero (128-bit intermediate).
     *
     * Saturates at ±max() instead of wrapping, so a notional computed from absurd inputs can never
     * turn into a valid-looking negative price. Check with `isSaturated()` if the distinction matters.
     */
    [[nodiscard]] Decimal mul(Decimal o) const noexcept;
    /// Quotient of two decimals, truncated toward zero; zero when dividing by zero; saturating.
    [[nodiscard]] Decimal div(Decimal o) const noexcept;

    /// Largest / smallest representable value (±92 233 720 368.54775807).
    [[nodiscard]] static constexpr Decimal max() noexcept { return Decimal{9'223'372'036'854'775'807LL}; }
    [[nodiscard]] static constexpr Decimal min() noexcept { return Decimal{-9'223'372'036'854'775'807LL - 1}; }
    /// True when the value sits at a representation limit (a saturated arithmetic result).
    [[nodiscard]] constexpr bool isSaturated() const noexcept { return raw_ == max().raw_ || raw_ == min().raw_; }

    constexpr auto operator<=>(const Decimal&) const noexcept = default;
    constexpr bool operator==(const Decimal&) const noexcept = default;

private:
    constexpr explicit Decimal(std::int64_t raw) noexcept : raw_(raw) {}

    std::int64_t raw_{0};
};

/// Stream the wire form (for logging and test diagnostics).
std::ostream& operator<<(std::ostream& os, Decimal value);

/// Rounding direction used by the price/size helpers.
enum class RoundingMode : std::uint8_t {
    Down,     ///< toward negative infinity (but see AssetInfo::roundSz, which rounds toward zero)
    Up,       ///< toward positive infinity
    Nearest,  ///< half away from zero
};

/// Round @p value to a multiple of @p quantumRaw (a raw mantissa, > 0).
[[nodiscard]] Decimal roundToQuantum(Decimal value, std::int64_t quantumRaw, RoundingMode mode) noexcept;

}  // namespace hl
