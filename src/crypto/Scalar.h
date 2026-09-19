// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

// Arithmetic modulo the secp256k1 group order
//   n = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C D0364141
// on 4×64-bit little-endian limbs. Used by the precomputed-nonce ECDSA signer,
// whose online step is s = k⁻¹·(z + r·d) mod n.
//
// Reduction folds the part above 2²⁵⁶ using 2²⁵⁶ ≡ C (mod n), C = 2²⁵⁶ − n
// (129 bits). All loops have data-independent iteration counts and the final
// correction uses masks, not branches.

#include <array>
#include <cstdint>
#include <cstring>

#include "hl/core/Int128.h"

namespace hl::detail {

using Limbs = std::array<std::uint64_t, 4>;
__extension__ typedef unsigned __int128 U128;

inline constexpr Limbs kOrderN = {0xBFD25E8CD0364141ULL, 0xBAAEDCE6AF48A03BULL, 0xFFFFFFFFFFFFFFFEULL,
                                  0xFFFFFFFFFFFFFFFFULL};
inline constexpr Limbs kOrderHalf = {0xDFE92F46681B20A0ULL, 0x5D576E7357A4501DULL, 0xFFFFFFFFFFFFFFFFULL,
                                     0x7FFFFFFFFFFFFFFFULL};
// C = 2^256 - n = 0x1_4551231950B75FC4_402DA1732FC9BEBF
inline constexpr std::uint64_t kC0 = 0x402DA1732FC9BEBFULL;
inline constexpr std::uint64_t kC1 = 0x4551231950B75FC4ULL;

/// Big-endian 32 bytes → limbs (no reduction).
inline Limbs fromBytes(const std::uint8_t* be) noexcept {
    Limbs l{};
    for (int i = 0; i < 4; ++i) {
        std::uint64_t v = 0;
        for (int b = 0; b < 8; ++b) {
            v = (v << 8) | be[(3 - i) * 8 + b];
        }
        l[static_cast<std::size_t>(i)] = v;
    }
    return l;
}

/// Limbs → big-endian 32 bytes.
inline void toBytes(const Limbs& l, std::uint8_t* be) noexcept {
    for (int i = 0; i < 4; ++i) {
        std::uint64_t v = l[static_cast<std::size_t>(i)];
        for (int b = 7; b >= 0; --b) {
            be[(3 - i) * 8 + b] = static_cast<std::uint8_t>(v);
            v >>= 8;
        }
    }
}

/// r = a − b; returns the borrow (0/1).
inline std::uint64_t sub(const Limbs& a, const Limbs& b, Limbs& r) noexcept {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const U128 d = static_cast<U128>(a[i]) - b[i] - borrow;
        r[i] = static_cast<std::uint64_t>(d);
        borrow = static_cast<std::uint64_t>(d >> 64) & 1U;
    }
    return borrow;
}

/// Constant-time: a < 2·n → a mod n.
inline Limbs condSubN(const Limbs& a) noexcept {
    Limbs t{};
    const std::uint64_t borrow = sub(a, kOrderN, t);
    const std::uint64_t keepA = 0ULL - borrow;  // all ones when a < n
    Limbs r{};
    for (std::size_t i = 0; i < 4; ++i) {
        r[i] = (a[i] & keepA) | (t[i] & ~keepA);
    }
    return r;
}

/// Constant-time a < b.
inline bool lessThan(const Limbs& a, const Limbs& b) noexcept {
    Limbs t{};
    return sub(a, b, t) != 0;
}

/// 192-bit column accumulator (w0 + w1·2^64 + w2·2^128) with branch-free carries.
struct Acc {
    std::uint64_t w0{0}, w1{0}, w2{0};

    void mul(std::uint64_t a, std::uint64_t b) noexcept {
        const U128 t = static_cast<U128>(a) * b;
        const auto lo = static_cast<std::uint64_t>(t);
        auto hi = static_cast<std::uint64_t>(t >> 64);
        w0 += lo;
        hi += static_cast<std::uint64_t>(w0 < lo);
        w1 += hi;
        w2 += static_cast<std::uint64_t>(w1 < hi);
    }
    void add(std::uint64_t a) noexcept {
        w0 += a;
        const auto over = static_cast<std::uint64_t>(w0 < a);
        w1 += over;
        w2 += static_cast<std::uint64_t>(w1 < over);
    }
    std::uint64_t extract() noexcept {
        const std::uint64_t out = w0;
        w0 = w1;
        w1 = w2;
        w2 = 0;
        return out;
    }
};

/// x (8 limbs, value < 2^512) → x mod n, using 2^256 ≡ C = kC0 + kC1·2^64 + 2^128 (mod n).
inline Limbs reduce512(const std::uint64_t* l) noexcept {
    const std::uint64_t h0 = l[4], h1 = l[5], h2 = l[6], h3 = l[7];
    Acc a;
    // Stage 1: m = l[0..3] + h·C  (≤ 386 bits → 7 limbs)
    a.add(l[0]); a.mul(h0, kC0);
    const std::uint64_t m0 = a.extract();
    a.add(l[1]); a.mul(h1, kC0); a.mul(h0, kC1);
    const std::uint64_t m1 = a.extract();
    a.add(l[2]); a.mul(h2, kC0); a.mul(h1, kC1); a.add(h0);
    const std::uint64_t m2 = a.extract();
    a.add(l[3]); a.mul(h3, kC0); a.mul(h2, kC1); a.add(h1);
    const std::uint64_t m3 = a.extract();
    a.mul(h3, kC1); a.add(h2);
    const std::uint64_t m4 = a.extract();
    a.add(h3);
    const std::uint64_t m5 = a.extract();
    const std::uint64_t m6 = a.extract();
    // Stage 2: p = m[0..3] + (m4, m5, m6)·C  (≤ 259 bits → 4 limbs + p4 ≤ 7)
    a.add(m0); a.mul(m4, kC0);
    const std::uint64_t p0 = a.extract();
    a.add(m1); a.mul(m5, kC0); a.mul(m4, kC1);
    const std::uint64_t p1 = a.extract();
    a.add(m2); a.mul(m6, kC0); a.mul(m5, kC1); a.add(m4);
    const std::uint64_t p2 = a.extract();
    a.add(m3); a.mul(m6, kC1); a.add(m5);
    const std::uint64_t p3 = a.extract();
    a.add(m6);
    const std::uint64_t p4 = a.extract();
    // Stage 3: r = p[0..3] + p4·C, then fold the final carry bit once more.
    Limbs r{};
    U128 t = static_cast<U128>(p0) + static_cast<U128>(p4) * kC0;
    r[0] = static_cast<std::uint64_t>(t);
    t = static_cast<U128>(p1) + static_cast<U128>(p4) * kC1 + static_cast<std::uint64_t>(t >> 64);
    r[1] = static_cast<std::uint64_t>(t);
    t = static_cast<U128>(p2) + p4 + static_cast<std::uint64_t>(t >> 64);
    r[2] = static_cast<std::uint64_t>(t);
    t = static_cast<U128>(p3) + static_cast<std::uint64_t>(t >> 64);
    r[3] = static_cast<std::uint64_t>(t);
    const auto overflow = static_cast<std::uint64_t>(t >> 64);
    t = static_cast<U128>(r[0]) + (overflow * kC0);
    r[0] = static_cast<std::uint64_t>(t);
    t = static_cast<U128>(r[1]) + (overflow * kC1) + static_cast<std::uint64_t>(t >> 64);
    r[1] = static_cast<std::uint64_t>(t);
    t = static_cast<U128>(r[2]) + overflow + static_cast<std::uint64_t>(t >> 64);
    r[2] = static_cast<std::uint64_t>(t);
    r[3] += static_cast<std::uint64_t>(t >> 64);
    return condSubN(r);
}

/// (a · b) mod n for a, b < n.
inline Limbs mulMod(const Limbs& a, const Limbs& b) noexcept {
    std::uint64_t prod[8] = {};
    for (std::size_t i = 0; i < 4; ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const U128 t = static_cast<U128>(a[i]) * b[j] + prod[i + j] + carry;
            prod[i + j] = static_cast<std::uint64_t>(t);
            carry = static_cast<std::uint64_t>(t >> 64);
        }
        prod[i + 4] = carry;
    }
    return reduce512(prod);
}

/// (a + b) mod n for a, b < n (the sum is < 2n: one conditional subtraction, carry included).
inline Limbs addMod(const Limbs& a, const Limbs& b) noexcept {
    Limbs sum{};
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const U128 t = static_cast<U128>(a[i]) + b[i] + carry;
        sum[i] = static_cast<std::uint64_t>(t);
        carry = static_cast<std::uint64_t>(t >> 64);
    }
    Limbs diff{};
    const std::uint64_t borrow = sub(sum, kOrderN, diff);
    // Keep the raw sum only if it did not overflow 2^256 and is below n.
    const std::uint64_t keepSum = 0ULL - (borrow & (1U ^ carry));
    Limbs r{};
    for (std::size_t i = 0; i < 4; ++i) {
        r[i] = (sum[i] & keepSum) | (diff[i] & ~keepSum);
    }
    return r;
}

/// Any 256-bit value → value mod n.
inline Limbs reduce256(const Limbs& a) noexcept { return condSubN(a); }

/// a⁻¹ mod n by Fermat; the caller must ensure a ≠ 0 (a == 0 yields 0, which is not an inverse).
/// Fermat, a^(n−2), with a fixed 4-bit window and masked table lookups.
inline Limbs invMod(const Limbs& a) noexcept {
    Limbs e{};
    sub(kOrderN, Limbs{2, 0, 0, 0}, e);
    std::array<Limbs, 16> table{};
    table[0] = Limbs{1, 0, 0, 0};
    for (std::size_t i = 1; i < 16; ++i) {
        table[i] = mulMod(table[i - 1], a);
    }
    Limbs result{1, 0, 0, 0};
    for (int window = 63; window >= 0; --window) {
        for (int sq = 0; sq < 4; ++sq) {
            result = mulMod(result, result);
        }
        const std::uint64_t digit = (e[static_cast<std::size_t>(window / 16)] >> ((window % 16) * 4)) & 0xF;
        Limbs factor{};
        for (std::uint64_t i = 0; i < 16; ++i) {
            const std::uint64_t match = 0ULL - static_cast<std::uint64_t>(i == digit);
            for (std::size_t w = 0; w < 4; ++w) {
                factor[w] |= table[i][w] & match;
            }
        }
        result = mulMod(result, factor);
    }
    // The table holds powers of the secret scalar; do not leave them on the stack.
    std::memset(static_cast<void*>(table.data()), 0, sizeof(table));
    return result;
}

[[nodiscard]] inline bool isZero(const Limbs& a) noexcept { return (a[0] | a[1] | a[2] | a[3]) == 0; }

}  // namespace hl::detail
