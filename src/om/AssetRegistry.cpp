// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/om/AssetRegistry.h"

#include <algorithm>
#include <array>

#include "json/Json.h"

namespace hl {

namespace {

constexpr std::array<std::int64_t, 19> kPow10 = {
    1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL, 100000000LL, 1000000000LL,
    10000000000LL, 100000000000LL, 1000000000000LL, 10000000000000LL, 100000000000000LL,
    1000000000000000LL, 10000000000000000LL, 100000000000000000LL, 1000000000000000000LL,
};

// Number of decimal digits of |raw| (raw != 0).
int digitCount(std::uint64_t v) noexcept {
    int n = 1;
    while (n < 19 && v >= static_cast<std::uint64_t>(kPow10[static_cast<std::size_t>(n)])) {
        ++n;
    }
    return n;
}

}  // namespace

Decimal AssetInfo::roundPx(Decimal px, RoundingMode mode) const noexcept {
    if (px.isZero()) {
        return px;
    }
    const std::uint64_t mag = px.raw() < 0 ? static_cast<std::uint64_t>(-px.raw()) : static_cast<std::uint64_t>(px.raw());
    // Position of the most significant digit relative to the decimal point:
    // value 75951.0 → raw 7595100000000 (13 digits) → exponent 13 - 1 - 8 = 4.
    const int msdExponent = digitCount(mag) - 1 - Decimal::kDecimals;
    int decimalsBySigFigs = 4 - msdExponent;  // 5 significant figures
    if (decimalsBySigFigs < 0) {
        decimalsBySigFigs = 0;  // integer prices are always allowed
    }
    int decimals = std::min(decimalsBySigFigs, std::max(0, maxPriceDecimals()));
    decimals = std::min(decimals, Decimal::kDecimals);
    const std::int64_t quantum = kPow10[static_cast<std::size_t>(Decimal::kDecimals - decimals)];
    return roundToQuantum(px, quantum, mode);
}

Decimal AssetInfo::roundSz(Decimal sz, RoundingMode mode) const noexcept {
    const int decimals = std::clamp(szDecimals, 0, Decimal::kDecimals);
    const std::int64_t quantum = kPow10[static_cast<std::size_t>(Decimal::kDecimals - decimals)];
    if (mode == RoundingMode::Down && sz.isNegative()) {
        return -roundToQuantum(-sz, quantum, RoundingMode::Down);  // toward zero
    }
    return roundToQuantum(sz, quantum, mode);
}

Result<std::size_t> AssetRegistry::loadPerpMeta(std::string_view text) {
    json::Document doc;
    if (!doc.parse(text)) {
        return Error{Error::Kind::Parse, 0, "meta: invalid JSON"};
    }
    auto universe = doc.root().field("universe");
    if (!universe.isArray()) {
        return Error{Error::Kind::Parse, 0, "meta: missing universe"};
    }
    std::erase_if(assets_, [](const AssetInfo& a) { return a.kind == AssetInfo::Kind::Perp; });
    std::uint32_t index = 0;
    std::size_t added = 0;
    for (auto item : universe.array()) {
        AssetInfo info;
        info.kind = AssetInfo::Kind::Perp;
        info.asset = index++;
        info.name = std::string{item.field("name").asString()};
        info.szDecimals = static_cast<int>(item.field("szDecimals").asInt());
        info.maxLeverage = static_cast<std::uint32_t>(item.field("maxLeverage").asInt());
        info.onlyIsolated = item.field("onlyIsolated").asBool();
        info.isDelisted = item.field("isDelisted").asBool();
        if (!info.name.empty()) {
            assets_.push_back(std::move(info));
            ++added;
        }
    }
    return added;
}

Result<std::size_t> AssetRegistry::loadSpotMeta(std::string_view text) {
    json::Document doc;
    if (!doc.parse(text)) {
        return Error{Error::Kind::Parse, 0, "spotMeta: invalid JSON"};
    }
    auto tokens = doc.root().field("tokens");
    auto universe = doc.root().field("universe");
    if (!tokens.isArray() || !universe.isArray()) {
        return Error{Error::Kind::Parse, 0, "spotMeta: missing tokens/universe"};
    }
    // token index → (szDecimals, name)
    std::vector<int> tokenSzDecimals;
    std::vector<std::string> tokenNames;
    for (auto token : tokens.array()) {
        const auto idx = static_cast<std::size_t>(token.field("index").asInt());
        if (tokenSzDecimals.size() <= idx) {
            tokenSzDecimals.resize(idx + 1, 0);
            tokenNames.resize(idx + 1);
        }
        tokenSzDecimals[idx] = static_cast<int>(token.field("szDecimals").asInt());
        tokenNames[idx] = std::string{token.field("name").asString()};
    }
    std::erase_if(assets_, [](const AssetInfo& a) { return a.kind == AssetInfo::Kind::Spot; });
    std::size_t added = 0;
    for (auto pair : universe.array()) {
        AssetInfo info;
        info.kind = AssetInfo::Kind::Spot;
        info.name = std::string{pair.field("name").asString()};
        info.asset = 10000U + static_cast<std::uint32_t>(pair.field("index").asInt());
        auto pairTokens = pair.field("tokens");
        if (pairTokens.isArray()) {
            for (auto t : pairTokens.array()) {
                const auto base = static_cast<std::size_t>(t.asInt());
                info.szDecimals = base < tokenSzDecimals.size() ? tokenSzDecimals[base] : 0;
                info.baseToken = base < tokenNames.size() ? tokenNames[base] : std::string{};
                break;  // the first token of the pair is the base asset
            }
        }
        if (!info.name.empty()) {
            assets_.push_back(std::move(info));
            ++added;
        }
    }
    return added;
}

void AssetRegistry::add(AssetInfo info) {
    auto it = std::find_if(assets_.begin(), assets_.end(), [&](const AssetInfo& a) { return a.name == info.name; });
    if (it != assets_.end()) {
        *it = std::move(info);
    } else {
        assets_.push_back(std::move(info));
    }
}

const AssetInfo* AssetRegistry::find(std::string_view name) const noexcept {
    for (const auto& a : assets_) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

const AssetInfo* AssetRegistry::findByAsset(std::uint32_t asset) const noexcept {
    for (const auto& a : assets_) {
        if (a.asset == asset) {
            return &a;
        }
    }
    return nullptr;
}

}  // namespace hl
