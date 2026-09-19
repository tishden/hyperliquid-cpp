// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include <benchmark/benchmark.h>

#include <string>

#include "hl/net/WsCodec.h"
#include "support/Fixtures.h"

namespace {

struct Sink final : hl::WsFrameSink {
    std::size_t bytes{0};
    void onWsMessage(hl::WsOpcode, std::string_view p) override { bytes += p.size(); }
    void onWsControl(hl::WsOpcode, std::string_view) override {}
};

std::string serverFrame(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(payload.size()));
    } else {
        f.push_back(126);
        f.push_back(static_cast<char>(payload.size() >> 8));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    return f + payload;
}

void WsDecode(benchmark::State& state, const std::string& payload) {
    const std::string frame = serverFrame(payload);
    hl::WsFrameDecoder decoder;
    Sink sink;
    for (auto _ : state) {
        decoder.feed(frame.data(), frame.size(), sink);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * frame.size()));
}

void WsDecode_Bbo(benchmark::State& s) { WsDecode(s, hltest::fixture("bbo_btc.json")); }
void WsDecode_L2Book(benchmark::State& s) { WsDecode(s, hltest::fixture("l2book_btc.json")); }

void WsEncode_OrderPost(benchmark::State& state) {
    const std::string payload(620, 'x');  // typical signed order post frame
    std::string out;
    out.reserve(1024);
    for (auto _ : state) {
        out.clear();
        hl::encodeWsFrame(hl::WsOpcode::Text, payload, 0xA5A5A5A5, out);
        benchmark::DoNotOptimize(out.data());
    }
}

}  // namespace

BENCHMARK(WsDecode_Bbo);
BENCHMARK(WsDecode_L2Book);
BENCHMARK(WsEncode_OrderPost);
