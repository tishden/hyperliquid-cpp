// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
// Cross-checks the private mod-n scalar arithmetic against OpenSSL BIGNUM on random inputs.
#include <gtest/gtest.h>

#include <openssl/bn.h>
#include <openssl/rand.h>

#include <memory>

#include "crypto/Scalar.h"

using namespace hl::detail;

namespace {

struct BnCtx {
    BN_CTX* ctx = BN_CTX_new();
    ~BnCtx() { BN_CTX_free(ctx); }
};

using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

BnPtr bn(const Limbs& l) {
    std::uint8_t be[32];
    toBytes(l, be);
    return BnPtr(BN_bin2bn(be, 32, nullptr), &BN_free);
}

Limbs limbs(const BIGNUM* b) {
    std::uint8_t be[32] = {};
    BN_bn2binpad(b, be, 32);
    return fromBytes(be);
}

Limbs randomBelowN() {
    for (;;) {
        std::uint8_t be[32];
        RAND_bytes(be, 32);
        Limbs l = fromBytes(be);
        if (lessThan(l, kOrderN)) {
            return l;
        }
    }
}

Limbs edge(int i) {
    Limbs nMinus1{};
    sub(kOrderN, Limbs{1, 0, 0, 0}, nMinus1);
    switch (i % 5) {
        case 0: return Limbs{0, 0, 0, 0};
        case 1: return Limbs{1, 0, 0, 0};
        case 2: return nMinus1;
        case 3: return kOrderHalf;
        default: return Limbs{~0ULL, ~0ULL, 0, 0};
    }
}

}  // namespace

TEST(Scalar, BytesRoundTrip) {
    const Limbs a = randomBelowN();
    std::uint8_t be[32];
    toBytes(a, be);
    EXPECT_EQ(fromBytes(be), a);
}

TEST(Scalar, MulAddMatchBignum) {
    BnCtx c;
    const BnPtr n = bn(kOrderN);
    for (int i = 0; i < 20000; ++i) {
        const Limbs a = i < 25 ? edge(i) : randomBelowN();
        const Limbs b = i < 25 ? edge(i / 5) : randomBelowN();
        BnPtr expected(BN_new(), &BN_free);
        ASSERT_EQ(BN_mod_mul(expected.get(), bn(a).get(), bn(b).get(), n.get(), c.ctx), 1);
        ASSERT_EQ(mulMod(a, b), limbs(expected.get())) << i;
        ASSERT_EQ(BN_mod_add(expected.get(), bn(a).get(), bn(b).get(), n.get(), c.ctx), 1);
        ASSERT_EQ(addMod(a, b), limbs(expected.get())) << i;
    }
}

TEST(Scalar, ReduceAnyValue) {
    BnCtx c;
    const BnPtr n = bn(kOrderN);
    for (int i = 0; i < 2000; ++i) {
        std::uint8_t be[32];
        RAND_bytes(be, 32);
        if (i < 32) {
            std::memset(be, 0xFF, 32);  // near 2^256
            be[31] = static_cast<std::uint8_t>(0xFF - i);
        }
        const Limbs a = fromBytes(be);
        BnPtr expected(BN_new(), &BN_free);
        ASSERT_EQ(BN_nnmod(expected.get(), bn(a).get(), n.get(), c.ctx), 1);
        ASSERT_EQ(reduce256(a), limbs(expected.get()));
    }
}

TEST(Scalar, InverseMatchesBignum) {
    BnCtx c;
    const BnPtr n = bn(kOrderN);
    for (int i = 0; i < 200; ++i) {
        const Limbs a = i == 0 ? Limbs{1, 0, 0, 0} : randomBelowN();
        if (isZero(a)) {
            continue;
        }
        BnPtr expected(BN_mod_inverse(nullptr, bn(a).get(), n.get(), c.ctx), &BN_free);
        ASSERT_EQ(invMod(a), limbs(expected.get()));
        EXPECT_EQ(mulMod(a, invMod(a)), (Limbs{1, 0, 0, 0}));
    }
}

TEST(Scalar, HalfOrderConstant) {
    Limbs twice = addMod(kOrderHalf, kOrderHalf);  // 2·⌊n/2⌋ = n − 1
    Limbs nMinus1{};
    sub(kOrderN, Limbs{1, 0, 0, 0}, nMinus1);
    EXPECT_EQ(twice, nMinus1);
}
