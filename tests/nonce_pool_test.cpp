// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
// Precomputed-nonce ECDSA: every signature must be valid, low-s, recover to the signer,
// and no nonce may ever be used twice.
#include <gtest/gtest.h>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "hl/core/Hex.h"
#include "hl/crypto/Signer.h"

namespace {

constexpr std::string_view kKey = "0x0123456789012345678901234567890123456789012345678901234567890123";

// Recover the Ethereum address from (digest, r, s, v) and check signature validity.
hl::Address recoverAddress(const hl::Hash256& digest, const hl::Signature& sig) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    std::uint8_t compact[64];
    std::copy(sig.r.begin(), sig.r.end(), compact);
    std::copy(sig.s.begin(), sig.s.end(), compact + 32);
    secp256k1_ecdsa_recoverable_signature rsig;
    EXPECT_EQ(secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &rsig, compact, sig.v - 27), 1);
    secp256k1_pubkey pub;
    EXPECT_EQ(secp256k1_ecdsa_recover(ctx, &pub, &rsig, digest.data()), 1);
    secp256k1_ecdsa_signature plain;
    secp256k1_ecdsa_recoverable_signature_convert(ctx, &plain, &rsig);
    EXPECT_EQ(secp256k1_ecdsa_verify(ctx, &plain, digest.data(), &pub), 1);  // also rejects high-s
    std::uint8_t ser[65];
    std::size_t len = sizeof(ser);
    secp256k1_ec_pubkey_serialize(ctx, ser, &len, &pub, SECP256K1_EC_UNCOMPRESSED);
    secp256k1_context_destroy(ctx);
    const auto h = hl::keccak256(ser + 1, 64);
    hl::Address a{};
    std::copy(h.begin() + 12, h.end(), a.begin());
    return a;
}

hl::Hash256 digestOf(int i) { return hl::keccak256("message-" + std::to_string(i)); }

}  // namespace

TEST(NoncePool, SignaturesRecoverToSigner) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(256, /*backgroundThread=*/false);
    ASSERT_EQ(signer.refillNonces(1000), 256U);
    EXPECT_EQ(signer.noncePoolSize(), 256U);
    std::set<std::string> rs;
    for (int i = 0; i < 256; ++i) {
        const auto d = digestOf(i);
        const auto sig = signer.signDigest(d);
        ASSERT_TRUE(sig.v == 27 || sig.v == 28);
        ASSERT_LT(sig.s[0], 0x80) << "high-s";
        ASSERT_EQ(recoverAddress(d, sig), signer.address()) << i;
        rs.insert(hl::toHex(sig.r.data(), 32));
    }
    EXPECT_EQ(rs.size(), 256U) << "nonce reuse";
    EXPECT_EQ(signer.noncePoolSize(), 0U);
    EXPECT_EQ(signer.signingStats().precomputed, 256U);
    EXPECT_EQ(signer.signingStats().deterministic, 0U);
}

TEST(NoncePool, SameDigestSignedTwiceDiffers) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(4, false);
    signer.refillNonces(4);
    const auto d = digestOf(1);
    const auto a = signer.signDigest(d);
    const auto b = signer.signDigest(d);
    EXPECT_NE(a.r, b.r);
    EXPECT_EQ(recoverAddress(d, a), signer.address());
    EXPECT_EQ(recoverAddress(d, b), signer.address());
}

TEST(NoncePool, EmptyPoolFallsBackToDeterministic) {
    hl::Signer plain{kKey};
    hl::Signer pooled{kKey};
    pooled.enableNoncePool(8, false);  // never refilled
    const auto d = digestOf(7);
    const auto expected = plain.signDigest(d);
    const auto got = pooled.signDigest(d);
    EXPECT_EQ(got.r, expected.r);
    EXPECT_EQ(got.s, expected.s);
    EXPECT_EQ(got.v, expected.v);
    EXPECT_EQ(pooled.signingStats().deterministic, 1U);
}

TEST(NoncePool, DisableWipesAndCapacityRoundsUp) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(100, false);
    EXPECT_EQ(signer.refillNonces(1000), 128U);  // rounded to a power of two
    signer.disableNoncePool();
    EXPECT_EQ(signer.noncePoolSize(), 0U);
    EXPECT_EQ(signer.refillNonces(10), 0U);
    signer.enableNoncePool(0);
    EXPECT_EQ(signer.noncePoolSize(), 0U);
}

TEST(NoncePool, ConcurrentSignersWithBackgroundRefillNeverReuseNonces) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(64, /*backgroundThread=*/true);
    std::mutex mutex;
    std::set<std::string> rs;
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 400;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto d = digestOf(t * 100000 + i);
                const auto sig = signer.signDigest(d);
                if (recoverAddress(d, sig) != signer.address()) {
                    ++bad;
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (!rs.insert(hl::toHex(sig.r.data(), 32)).second) {
                    ++bad;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(rs.size(), static_cast<std::size_t>(kThreads * kPerThread));
    const auto stats = signer.signingStats();
    EXPECT_EQ(stats.precomputed + stats.deterministic, static_cast<std::uint64_t>(kThreads * kPerThread));
    EXPECT_GT(stats.precomputed, 0U);
}

TEST(NoncePool, BackgroundThreadFillsPool) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(32, true);
    for (int i = 0; i < 500 && signer.noncePoolSize() < 32; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(signer.noncePoolSize(), 32U);
}

TEST(NoncePool, L1ActionSignatureRecovers) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(2, false);
    signer.refillNonces(2);
    const std::vector<std::uint8_t> action = {0x81, 0xa4, 't', 'y', 'p', 'e'};
    const auto sig = signer.signL1Action(action, std::nullopt, 1700000000000ULL, std::nullopt, false);
    const auto digest = hl::agentDigest(hl::actionHash(action, std::nullopt, 1700000000000ULL, std::nullopt), false);
    EXPECT_EQ(recoverAddress(digest, sig), signer.address());
    EXPECT_EQ(signer.signingStats().precomputed, 1U);
}
