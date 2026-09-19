// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include <benchmark/benchmark.h>

#include <array>
#include <cstdlib>
#include <string>

#include "hl/core/Decimal.h"

namespace {

constexpr std::array<std::string_view, 6> kInputs = {"75951.0", "0.82385", "2390.7", "36851.8675", "-0.0001711314", "12"};

void Decimal_Parse(benchmark::State& state) {
    std::size_t i = 0;
    hl::Decimal d;
    for (auto _ : state) {
        benchmark::DoNotOptimize(hl::Decimal::parse(kInputs[i++ % kInputs.size()], d));
    }
}

void Strtod_Baseline(benchmark::State& state) {
    std::array<std::string, kInputs.size()> owned;
    for (std::size_t k = 0; k < kInputs.size(); ++k) {
        owned[k] = std::string{kInputs[k]};
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(std::strtod(owned[i++ % owned.size()].c_str(), nullptr));
    }
}

void Decimal_ToString(benchmark::State& state) {
    const hl::Decimal d = hl::Decimal::parseOrZero("75951.37");
    std::string out;
    out.reserve(32);
    for (auto _ : state) {
        out.clear();
        d.appendTo(out);
        benchmark::DoNotOptimize(out.data());
    }
}

}  // namespace

BENCHMARK(Decimal_Parse);
BENCHMARK(Strtod_Baseline);
BENCHMARK(Decimal_ToString);
