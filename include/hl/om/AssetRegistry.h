#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hl/core/Decimal.h"
#include "hl/core/Result.h"
#include "hl/core/Types.h"

namespace hl {

/**
 * @brief Static trading parameters of one tradable asset, plus the HL rounding rules.
 *
 * Hyperliquid has no explicit tick size. A price is valid when it has
 *  - at most **5 significant figures** (integer prices are always valid), and
 *  - at most `6 − szDecimals` decimals on perps / `8 − szDecimals` on spot.
 * A size is valid when it has at most `szDecimals` decimals. Orders that break
 * these rules are rejected by the venue, so always round through this class.
 */
struct AssetInfo {
    enum class Kind : std::uint8_t { Perp, Spot };

    std::string name{};          ///< coin as used in subscriptions and fills: "BTC", "PURR/USDC", "@107"
    std::uint32_t asset{};       ///< wire asset id: perp index, or 10000 + spot index
    Kind kind{Kind::Perp};
    int szDecimals{};            ///< size precision
    std::string baseToken{};     ///< spot only: base token of the pair ("PURR" for "PURR/USDC")
    std::uint32_t maxLeverage{}; ///< perps only
    bool onlyIsolated{false};    ///< perps only
    bool isDelisted{false};      ///< perps only

    /// Max decimals a price may have on this asset, before the sig-fig rule.
    [[nodiscard]] int maxPriceDecimals() const noexcept { return (kind == Kind::Perp ? 6 : 8) - szDecimals; }

    /// Round a price to the nearest valid value in the given direction.
    [[nodiscard]] Decimal roundPx(Decimal px, RoundingMode mode = RoundingMode::Nearest) const noexcept;
    /**
     * @brief Round a size to `szDecimals`.
     *
     * With `RoundingMode::Down` a negative size rounds **toward zero**, not toward negative
     * infinity, so rounding never asks for more than the caller requested on either side.
     */
    [[nodiscard]] Decimal roundSz(Decimal sz, RoundingMode mode = RoundingMode::Down) const noexcept;
    /// True when the price already satisfies both rules.
    [[nodiscard]] bool isValidPx(Decimal px) const noexcept { return roundPx(px, RoundingMode::Down) == px; }
    /// True when the size already satisfies the size rule.
    [[nodiscard]] bool isValidSz(Decimal sz) const noexcept { return roundSz(sz) == sz; }
};

/**
 * @brief Name → asset lookup built from the `meta` and `spotMeta` info responses.
 *
 * Loaded once at start-up by ExchangeClient (or manually via `loadPerpMeta`
 * / `loadSpotMeta` with raw JSON). Lookups are linear over a few hundred
 * entries and happen on the control path only (order entry resolves the
 * asset once per call).
 */
class AssetRegistry {
public:
    /// Parse a `{"type":"meta"}` response (perp universe). Replaces existing perps.
    [[nodiscard]] Result<std::size_t> loadPerpMeta(std::string_view json);
    /// Parse a `{"type":"spotMeta"}` response. Replaces existing spot assets.
    [[nodiscard]] Result<std::size_t> loadSpotMeta(std::string_view json);

    /// Insert or replace one asset (useful for tests and static configuration).
    void add(AssetInfo info);

    [[nodiscard]] const AssetInfo* find(std::string_view name) const noexcept;
    [[nodiscard]] const AssetInfo* findByAsset(std::uint32_t asset) const noexcept;
    [[nodiscard]] const std::vector<AssetInfo>& all() const noexcept { return assets_; }
    [[nodiscard]] bool empty() const noexcept { return assets_.empty(); }

private:
    std::vector<AssetInfo> assets_;
};

}  // namespace hl
