// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/core/Hex.h"

#include <charconv>
#include <cstdio>

#include "hl/core/Types.h"

namespace hl {

namespace {
constexpr char kHexDigits[] = "0123456789abcdef";
}  // namespace

std::string toHex(const std::uint8_t* data, std::size_t len) {
    std::string out(len * 2, '0');
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i] = kHexDigits[data[i] >> 4];
        out[2 * i + 1] = kHexDigits[data[i] & 0x0F];
    }
    return out;
}

bool fromHex(std::string_view hex, std::uint8_t* out, std::size_t outLen) noexcept {
    hex = stripHexPrefix(hex);
    if (hex.size() != outLen * 2) {
        return false;
    }
    for (std::size_t i = 0; i < outLen; ++i) {
        const int hi = hexNibble(hex[2 * i]);
        const int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

// ── Types.h out-of-line definitions ─────────────────────────────────────────

std::string_view restUrl(Network network) noexcept {
    return network == Network::Mainnet ? "https://api.hyperliquid.xyz" : "https://api.hyperliquid-testnet.xyz";
}

std::string_view wsUrl(Network network) noexcept {
    return network == Network::Mainnet ? "wss://api.hyperliquid.xyz/ws" : "wss://api.hyperliquid-testnet.xyz/ws";
}

bool isValidCoinName(std::string_view coin) noexcept {
    if (coin.empty() || coin.size() > 64) {
        return false;
    }
    for (const char c : coin) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                             c == '@' || c == '-' || c == '_' || c == '.' || c == ':';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::string_view toString(Side side) noexcept { return side == Side::Buy ? "Buy" : "Sell"; }

std::string_view toString(Tif tif) noexcept {
    switch (tif) {
        case Tif::Alo: return "Alo";
        case Tif::Ioc: return "Ioc";
        case Tif::Gtc: return "Gtc";
    }
    return "Gtc";
}

std::string_view toString(Error::Kind kind) noexcept {
    switch (kind) {
        case Error::Kind::None: return "None";
        case Error::Kind::Transport: return "Transport";
        case Error::Kind::Timeout: return "Timeout";
        case Error::Kind::Http: return "Http";
        case Error::Kind::Venue: return "Venue";
        case Error::Kind::Parse: return "Parse";
        case Error::Kind::Rejected: return "Rejected";
    }
    return "Unknown";
}

std::optional<Address> parseAddress(std::string_view hex) noexcept {
    Address a{};
    if (!fromHex(hex, a.data(), a.size())) {
        return std::nullopt;
    }
    return a;
}

std::string toHex(const Address& address) { return "0x" + toHex(address.data(), address.size()); }

std::optional<Cloid> Cloid::parse(std::string_view hex) noexcept {
    hex = stripHexPrefix(hex);
    if (hex.size() != 32) {
        return std::nullopt;
    }
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const int n = hexNibble(hex[i]);
        if (n < 0) {
            return std::nullopt;
        }
        auto& part = i < 16 ? hi : lo;
        part = (part << 4) | static_cast<std::uint64_t>(n);
    }
    return Cloid{hi, lo};
}

std::string Cloid::toString() const {
    std::string out = "0x";
    out.resize(34);
    for (int i = 0; i < 16; ++i) {
        out[static_cast<std::size_t>(2 + i)] = kHexDigits[(high_ >> (60 - 4 * i)) & 0xF];
        out[static_cast<std::size_t>(18 + i)] = kHexDigits[(low_ >> (60 - 4 * i)) & 0xF];
    }
    return out;
}

}  // namespace hl
