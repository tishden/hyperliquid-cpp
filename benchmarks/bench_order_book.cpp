// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include <benchmark/benchmark.h>

#include <vector>

#include "hl/WsMessageParser.h"
#include "hl/md/OrderBook.h"
#include "support/Fixtures.h"

namespace {

struct Capture final : hl::WsMessageHandler {
    std::vector<hl::BookLevel> bids, asks;
    std::int64_t time{};
    void onL2Book(const hl::L2BookMsg& m) override {
        bids.assign(m.bids.begin(), m.bids.end());
        asks.assign(m.asks.begin(), m.asks.end());
        time = m.timeMs;
    }
};

Capture& snapshot() {
    static Capture c = [] {
        Capture cap;
        hl::WsMessageParser p;
        p.parse(hltest::fixture("l2book_btc.json"), cap);
        return cap;
    }();
    return c;
}

hl::L2BookMsg msg() {
    auto& c = snapshot();
    return hl::L2BookMsg{"BTC", c.time, c.bids, c.asks};
}

void Book_ApplySnapshot_20x20(benchmark::State& state) {
    hl::OrderBook book{"BTC"};
    const auto m = msg();
    for (auto _ : state) {
        book.applySnapshot(m);
        benchmark::DoNotOptimize(book.bestBid());
    }
}

void Book_ApplyBbo_TopResize(benchmark::State& state) {
    hl::OrderBook book{"BTC"};
    book.applySnapshot(msg());
    hl::BboMsg bbo;
    bbo.coin = "BTC";
    bbo.timeMs = msg().timeMs + 1;
    bbo.hasBid = bbo.hasAsk = true;
    bbo.bid = book.bids()[0];
    bbo.ask = book.asks()[0];
    std::int64_t i = 0;
    for (auto _ : state) {
        bbo.bid.sz = hl::Decimal::fromRaw(1'000'000 + (++i & 0xFFFF));
        benchmark::DoNotOptimize(book.applyBbo(bbo));
    }
}

void Book_ApplyBbo_PriceWalk(benchmark::State& state) {
    hl::OrderBook book{"BTC"};
    const auto m = msg();
    hl::BboMsg up;
    up.coin = "BTC";
    up.timeMs = m.timeMs + 1;
    up.hasBid = up.hasAsk = true;
    up.bid = hl::BookLevel{m.bids[0].px + hl::Decimal::fromInt(0), m.bids[0].sz, 1};
    up.ask = hl::BookLevel{m.asks[2].px, m.asks[2].sz, 1};  // ask side consumed two levels
    for (auto _ : state) {
        book.applySnapshot(m);
        benchmark::DoNotOptimize(book.applyBbo(up));
    }
}

void Book_Microprice(benchmark::State& state) {
    hl::OrderBook book{"BTC"};
    book.applySnapshot(msg());
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.microprice());
    }
}

void Book_VwapForSize_5Levels(benchmark::State& state) {
    hl::OrderBook book{"BTC"};
    book.applySnapshot(msg());
    const hl::Decimal size = book.cumulativeSize(hl::Side::Sell, 5);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.vwapForSize(hl::Side::Buy, size));
    }
}

}  // namespace

BENCHMARK(Book_ApplySnapshot_20x20);
BENCHMARK(Book_ApplyBbo_TopResize);
BENCHMARK(Book_ApplyBbo_PriceWalk);
BENCHMARK(Book_Microprice);
BENCHMARK(Book_VwapForSize_5Levels);
