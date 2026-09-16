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
        benchmark::DoNotOptimize(hl::RequestBuilder::wsPost(1, builder.payload(action, nonces.next())));
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
