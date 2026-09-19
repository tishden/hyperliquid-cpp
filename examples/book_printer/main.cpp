// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
// hl_book_printer — minimal market-data example: maintain order books for a few
// coins and print the top of book, spread, microprice and last trade.
//
//   ./hl_book_printer BTC ETH SOL            # mainnet
//   ./hl_book_printer --testnet BTC
//   ./hl_book_printer --fast BTC              # 5-level l2Book at ~10x the snapshot rate

#include <csignal>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "hl/hyperliquid.h"

namespace {

hl::EventLoop* gLoop = nullptr;

class Printer final : public hl::MarketDataListener {
public:
    void onConnected() override { std::printf("connected\n"); }
    void onDisconnected(std::string_view reason) override {
        std::printf("disconnected: %.*s\n", static_cast<int>(reason.size()), reason.data());
    }
    void onTrades(std::span<const hl::TradeMsg> trades) override {
        for (const auto& t : trades) {
            lastTrade_[std::string{t.coin}] = std::string{t.side == hl::Side::Buy ? "B " : "S "} + t.sz.toString() + " @ " + t.px.toString();
        }
    }
    void onAssetCtx(const hl::AssetCtxMsg& ctx) override {
        funding_[std::string{ctx.coin}] = ctx.funding.toDouble() * 100.0;
    }
    void print(const hl::MarketDataClient& md, const std::vector<std::string>& coins) {
        std::printf("\n%-6s %14s %12s %14s %12s %9s %14s %10s  %s\n", "coin", "bid", "bid sz", "ask", "ask sz", "spr bps",
                    "microprice", "fund %/h", "last trade");
        for (const auto& coin : coins) {
            const hl::OrderBook* b = md.book(coin);
            if (b == nullptr || !b->isValid()) {
                std::printf("%-6s (no book yet)\n", coin.c_str());
                continue;
            }
            std::printf("%-6s %14s %12s %14s %12s %9.2f %14s %10.5f  %s\n", coin.c_str(), b->bestBid()->px.toString().c_str(),
                        b->bestBid()->sz.toString().c_str(), b->bestAsk()->px.toString().c_str(),
                        b->bestAsk()->sz.toString().c_str(), b->spreadBps(), b->microprice().toString().c_str(),
                        funding_[coin], lastTrade_[coin].c_str());
        }
    }

private:
    std::map<std::string, std::string> lastTrade_;
    std::map<std::string, double> funding_;
};

void schedulePrint(hl::EventLoop& loop, Printer& printer, hl::MarketDataClient& md, const std::vector<std::string>& coins) {
    loop.addTimer(1000, [&] {
        printer.print(md, coins);
        schedulePrint(loop, printer, md, coins);
    });
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    hl::MarketDataConfig cfg;
    hl::L2BookOptions bookOptions;
    std::vector<std::string> coins;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--testnet") {
            cfg.network = hl::Network::Testnet;
        } else if (a == "--fast") {
            bookOptions.fast = true;
        } else {
            coins.push_back(a);
        }
    }
    if (coins.empty()) {
        coins = {"BTC", "ETH"};
    }
    hl::EventLoop loop;
    gLoop = &loop;
    std::signal(SIGINT, [](int) { gLoop->stop(); });

    Printer printer;
    hl::MarketDataClient md(loop, printer, cfg);
    for (const auto& c : coins) {
        md.subscribeBook(c, bookOptions);
        md.subscribeTrades(c);
        md.subscribeAssetCtx(c);
    }
    md.start();
    schedulePrint(loop, printer, md, coins);
    loop.run();
    const auto& st = md.parserStats();
    std::printf("\nmessages %llu, parse errors %llu, reconnects %llu\n", static_cast<unsigned long long>(st.messages),
                static_cast<unsigned long long>(st.parseErrors), static_cast<unsigned long long>(md.reconnectCount()));
    return 0;
}
