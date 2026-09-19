// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include <benchmark/benchmark.h>

#include <array>
#include <string>

#include "hl/crypto/Keccak.h"
#include "hl/crypto/Signer.h"
#include "hl/om/Actions.h"

namespace {

constexpr std::string_view kKey = "0x0123456789012345678901234567890123456789012345678901234567890123";

hl::OrderWire order() {
    hl::OrderWire o;
    o.asset = 3;
    o.isBuy = true;
    o.px = hl::Decimal::parseOrZero("75951");
    o.sz = hl::Decimal::parseOrZero("0.00026");
    o.tif = hl::Tif::Alo;
    o.cloid = hl::Cloid{0x1234, 42};
    return o;
}

void Keccak256_64B(benchmark::State& state) {
    const std::string data(64, 'x');
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::keccak256(data));
    }
}

void Keccak256_1KB(benchmark::State& state) {
    const std::string data(1024, 'x');
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::keccak256(data));
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * data.size()));
}

void Action_BuildOrder(benchmark::State& state) {
    const std::array<hl::OrderWire, 1> orders{order()};
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::actions::order(orders));
    }
}

void Action_Hash_EIP712(benchmark::State& state) {
    const std::array<hl::OrderWire, 1> orders{order()};
    const auto action = hl::actions::order(orders);
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::agentDigest(hl::actionHash(action.msgpack, std::nullopt, 1700000000000ULL, std::nullopt), false));
    }
}

void Sign_EcdsaSecp256k1(benchmark::State& state) {
    hl::Signer signer{kKey};
    const hl::Hash256 digest = hl::keccak256("digest");
    for (auto _ : state) {
        benchmark::DoNotOptimize(signer.signDigest(digest));
    }
}

void Order_EndToEnd_SignedPayload(benchmark::State& state) {
    hl::Signer signer{kKey};
    hl::RequestBuilder builder{signer, hl::Network::Testnet};
    hl::NonceGenerator nonces;
    const std::array<hl::OrderWire, 1> orders{order()};
    for (auto _ : state) {
        const auto action = hl::actions::order(orders);
        benchmark::DoNotOptimize(builder.wsPostAction(1, action, nonces.next()));
    }
}

void Cancel_EndToEnd_SignedPayload(benchmark::State& state) {
    hl::Signer signer{kKey};
    hl::RequestBuilder builder{signer, hl::Network::Testnet};
    hl::NonceGenerator nonces;
    const std::array<hl::CancelByCloidWire, 1> cancels{{{3, hl::Cloid{0x1234, 42}}}};
    for (auto _ : state) {
        const auto action = hl::actions::cancelByCloid(cancels);
        benchmark::DoNotOptimize(builder.payload(action, nonces.next()));
    }
}

}  // namespace

BENCHMARK(Keccak256_64B);
BENCHMARK(Keccak256_1KB);
BENCHMARK(Action_BuildOrder);
BENCHMARK(Action_Hash_EIP712);
BENCHMARK(Sign_EcdsaSecp256k1);
BENCHMARK(Order_EndToEnd_SignedPayload);
BENCHMARK(Cancel_EndToEnd_SignedPayload);

namespace {

// ── stage breakdown of order entry ──────────────────────────────────────────

void Stage1_BuildAction(benchmark::State& state) {
    const std::array<hl::OrderWire, 1> orders{order()};
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::actions::order(orders));
    }
}

void Stage2_ActionHash(benchmark::State& state) {
    const std::array<hl::OrderWire, 1> orders{order()};
    const auto action = hl::actions::order(orders);
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::actionHash(action.msgpack, std::nullopt, 1700000000000ULL, std::nullopt));
    }
    state.counters["msgpack_bytes"] = static_cast<double>(action.msgpack.size());
}

void Stage3_AgentDigest(benchmark::State& state) {
    const hl::Hash256 connId = hl::keccak256("connection");
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::agentDigest(connId, false));
    }
}

void Stage4_Payload_NoSign(benchmark::State& state) {
    hl::Signer signer{kKey};
    hl::RequestBuilder builder{signer, hl::Network::Testnet};
    const std::array<hl::OrderWire, 1> orders{order()};
    const auto action = hl::actions::order(orders);
    const auto payload = builder.payload(action, 1700000000000ULL);
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::RequestBuilder::wsPost(1, payload));
    }
    state.counters["frame_bytes"] = static_cast<double>(payload.size());
}

}  // namespace

BENCHMARK(Stage1_BuildAction);
BENCHMARK(Stage2_ActionHash);
BENCHMARK(Stage3_AgentDigest);
BENCHMARK(Stage4_Payload_NoSign);

namespace {

// ── precomputed-nonce path ──────────────────────────────────────────────────
// The pool is refilled outside the timed region whenever it runs dry.

constexpr std::size_t kPoolBatch = 4096;

void ensurePool(benchmark::State& state, hl::Signer& signer) {
    if (signer.noncePoolSize() == 0) {
        state.PauseTiming();
        signer.refillNonces(kPoolBatch);
        state.ResumeTiming();
    }
}

void Sign_PrecomputedNonce(benchmark::State& state) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(kPoolBatch, false);
    const hl::Hash256 digest = hl::keccak256("digest");
    for (auto _ : state) {
        ensurePool(state, signer);
        benchmark::DoNotOptimize(signer.signDigest(digest));
    }
    state.counters["deterministic_fallbacks"] = static_cast<double>(signer.signingStats().deterministic);
}

void Order_EndToEnd_Precomputed(benchmark::State& state) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(kPoolBatch, false);
    hl::RequestBuilder builder{signer, hl::Network::Testnet};
    hl::NonceGenerator nonces;
    const std::array<hl::OrderWire, 1> orders{order()};
    for (auto _ : state) {
        ensurePool(state, signer);
        const auto action = hl::actions::order(orders);
        benchmark::DoNotOptimize(builder.wsPostAction(1, action, nonces.next()));
    }
    state.counters["deterministic_fallbacks"] = static_cast<double>(signer.signingStats().deterministic);
}

void Cancel_EndToEnd_Precomputed(benchmark::State& state) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(kPoolBatch, false);
    hl::RequestBuilder builder{signer, hl::Network::Testnet};
    hl::NonceGenerator nonces;
    const std::array<hl::CancelByCloidWire, 1> cancels{{{3, hl::Cloid{0x1234, 42}}}};
    for (auto _ : state) {
        ensurePool(state, signer);
        const auto action = hl::actions::cancelByCloid(cancels);
        benchmark::DoNotOptimize(builder.wsPostAction(1, action, nonces.next()));
    }
}

void NoncePool_ProduceOne(benchmark::State& state) {
    hl::Signer signer{kKey};
    signer.enableNoncePool(1, false);
    for (auto _ : state) {
        signer.refillNonces(1);
        state.PauseTiming();
        (void)signer.signDigest(hl::Hash256{});  // drain
        state.ResumeTiming();
    }
}

}  // namespace

BENCHMARK(Sign_PrecomputedNonce);
BENCHMARK(Order_EndToEnd_Precomputed);
BENCHMARK(Cancel_EndToEnd_Precomputed);
BENCHMARK(NoncePool_ProduceOne);
