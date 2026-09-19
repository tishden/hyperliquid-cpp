// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hl {

/// 32-byte digest / scalar.
using Hash256 = std::array<std::uint8_t, 32>;

/**
 * @brief Ethereum Keccak-256 (original Keccak padding `0x01`, *not* NIST SHA3-256).
 *
 * Self-contained implementation of Keccak-f[1600] — no dependency on a
 * particular OpenSSL build. Incremental: `update()` any number of times, then
 * `finalize()` once.
 */
class Keccak256 {
public:
    Keccak256() noexcept = default;

    void update(const std::uint8_t* data, std::size_t len) noexcept;
    void update(std::string_view s) noexcept {
        update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }
    /// Pad, absorb and return the digest. The object must not be reused afterwards.
    [[nodiscard]] Hash256 finalize() noexcept;

private:
    static constexpr std::size_t kRate = 136;  // (1600 - 2*256) / 8

    std::array<std::uint64_t, 25> state_{};
    std::array<std::uint8_t, kRate> buffer_{};
    std::size_t buffered_{0};
};

/// One-shot Keccak-256.
[[nodiscard]] Hash256 keccak256(const std::uint8_t* data, std::size_t len) noexcept;
[[nodiscard]] inline Hash256 keccak256(std::string_view s) noexcept {
    return keccak256(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

}  // namespace hl
