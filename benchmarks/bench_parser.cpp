#include <benchmark/benchmark.h>

#include <string>
#include <vector>

#include "hl/WsMessageParser.h"
#include "support/Fixtures.h"

namespace {

struct Sink final : hl::WsMessageHandler {
    std::int64_t acc{0};
    void onL2Book(const hl::L2BookMsg& m) override { acc += m.bids[0].px.raw(); }
    void onBbo(const hl::BboMsg& m) override { acc += m.bid.px.raw(); }
    void onTrades(std::span<const hl::TradeMsg> t) override { acc += static_cast<std::int64_t>(t.size()); }
    void onAssetCtx(const hl::AssetCtxMsg& m) override { acc += m.markPx.raw(); }
    void onOrderUpdates(std::span<const hl::OrderUpdateMsg> u) override { acc += static_cast<std::int64_t>(u.size()); }
    void onUserFills(const hl::UserFillsMsg& m) override { acc += static_cast<std::int64_t>(m.fills.size()); }
};

void benchFrame(benchmark::State& state, const std::string& frame) {
    hl::WsMessageParser parser;
    Sink sink;
    for (auto _ : state) {
        benchmark::DoNotOptimize(parser.parse(frame, sink));
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * frame.size()));
    state.counters["frame_bytes"] = static_cast<double>(frame.size());
    benchmark::DoNotOptimize(sink.acc);
}

void Parse_L2Book_20x20(benchmark::State& s) { benchFrame(s, hltest::fixture("l2book_btc.json")); }
void Parse_Bbo(benchmark::State& s) { benchFrame(s, hltest::fixture("bbo_btc.json")); }
void Parse_Trades_30(benchmark::State& s) { benchFrame(s, hltest::fixture("trades_btc.json")); }
void Parse_ActiveAssetCtx(benchmark::State& s) { benchFrame(s, hltest::fixture("asset_ctx_btc.json")); }

void Parse_OrderUpdate(benchmark::State& s) {
    benchFrame(s, R"({"channel":"orderUpdates","data":[{"order":{"coin":"BTC","side":"B","limitPx":"75000.0","sz":"0.001",)"
                  R"("oid":123456789,"timestamp":1700000000000,"origSz":"0.002","cloid":"0x0000000000000000000000000000abcd"},)"
                  R"("status":"open","statusTimestamp":1700000000001}]})");
}

void Parse_UserFill(benchmark::State& s) {
    benchFrame(s, R"({"channel":"userFills","data":{"isSnapshot":false,"user":"0x14791697260e4c9a71f18484c9f997b308e59325",)"
                  R"("fills":[{"coin":"BTC","px":"75930.0","sz":"0.30618","side":"A","time":1789582997706,"startPosition":"0.30618",)"
                  R"("dir":"Close Long","closedPnl":"-17.115462","hash":"0x5cf72430b27b86b95e7004448ba47802043f00164d7ea58b00bfcf83717f60a4",)"
                  R"("oid":547034114966,"crossed":false,"fee":"2.51081","tid":51121598154065,"feeToken":"USDC"}]}})");
}

// Replay of a real 25-second mainnet capture (BTC+ETH l2Book, bbo, trades, activeAssetCtx).
void Parse_MainnetSessionReplay(benchmark::State& state) {
    const auto frames = hltest::fixtureLines("session_mainnet.jsonl");
    std::size_t bytes = 0;
    for (const auto& f : frames) {
        bytes += f.size();
    }
    hl::WsMessageParser parser;
    Sink sink;
    for (auto _ : state) {
        for (const auto& f : frames) {
            parser.parse(f, sink);
        }
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * bytes));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * frames.size()));
    state.counters["frames"] = static_cast<double>(frames.size());
}

}  // namespace

BENCHMARK(Parse_L2Book_20x20);
BENCHMARK(Parse_Bbo);
BENCHMARK(Parse_Trades_30);
BENCHMARK(Parse_ActiveAssetCtx);
BENCHMARK(Parse_OrderUpdate);
BENCHMARK(Parse_UserFill);
BENCHMARK(Parse_MainnetSessionReplay)->Unit(benchmark::kMillisecond);
