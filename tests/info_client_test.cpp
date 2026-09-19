// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
// InfoClient end-to-end over a mock HTTP server, using captured mainnet/testnet responses.
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "hl/core/Log.h"
#include "hl/om/InfoClient.h"
#include "support/Fixtures.h"
#include "support/MockVenue.h"

using hl::Decimal;
using hltest::runUntil;

namespace {

Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

class InfoClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        hl::setLogLevel(hl::LogLevel::Error);
        venue.onHttp = [this](const std::string& path, const std::string& body) -> hltest::MockVenue::HttpReply {
            lastPath = path;
            lastBody = body;
            return {status, response};
        };
    }

    /// Run one request to completion and return whether the callback fired.
    template <typename T>
    bool call(const std::function<void(hl::InfoClient::Callback<T>)>& invoke, hl::Result<T>& out) {
        std::optional<hl::Result<T>> received;
        invoke([&](const hl::Result<T>& r) { received = r; });
        const bool done = runUntil(loop, [&] { return received.has_value(); });
        if (done) {
            out = *received;
        }
        return done;
    }

    hl::EventLoop loop;
    hltest::MockVenue venue{loop};
    hl::InfoClient info{loop, venue.httpUrl()};
    std::string response{"[]"};
    int status{200};
    std::string lastPath;
    std::string lastBody;
    const hl::Address user = *hl::parseAddress("0x14791697260e4c9a71f18484c9f997b308e59325");
};

}  // namespace

TEST_F(InfoClientTest, PerpContextsJoinsMetaAndContexts) {
    response = hltest::fixture("meta_and_asset_ctxs.json");
    hl::Result<std::vector<hl::PerpContext>> r{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::PerpContext>>([&](auto cb) { info.perpContexts(cb); }, r));
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_EQ(r->size(), 5U);
    EXPECT_EQ(lastBody, R"({"type":"metaAndAssetCtxs"})");
    const auto& btc = (*r)[0];
    EXPECT_EQ(btc.coin, "BTC");
    EXPECT_EQ(btc.asset, 0U);
    EXPECT_EQ(btc.szDecimals, 5);
    EXPECT_GT(btc.maxLeverage, 0U);
    EXPECT_GT(btc.markPx, Decimal{});
    EXPECT_GT(btc.oraclePx, Decimal{});
    EXPECT_GT(btc.openInterest, Decimal{});
    EXPECT_GT(btc.dayNtlVlm, Decimal{});
    EXPECT_GT(btc.impactAsk, btc.impactBid);
    for (std::size_t i = 1; i < r->size(); ++i) {
        EXPECT_EQ((*r)[i].asset, static_cast<std::uint32_t>(i)) << "asset id must be the universe index";
        EXPECT_FALSE((*r)[i].coin.empty());
    }
}

TEST_F(InfoClientTest, Candles) {
    response = hltest::fixture("candles_eth_1m.json");
    hl::Result<std::vector<hl::Candle>> r{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::Candle>>(
        [&](auto cb) { info.candles("ETH", "1m", 1789800000000LL, 1789800300000LL, cb); }, r));
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_FALSE(r->empty());
    EXPECT_EQ(lastBody,
              R"({"type":"candleSnapshot","req":{"coin":"ETH","interval":"1m","startTime":1789800000000,"endTime":1789800300000}})");
    const auto& c = r->front();
    EXPECT_EQ(c.coin, "ETH");
    EXPECT_EQ(c.interval, "1m");
    EXPECT_EQ(c.openTimeMs, 1789800000000LL);
    EXPECT_EQ(c.closeTimeMs, 1789800059999LL);
    EXPECT_EQ(c.open, d("2629.4"));
    EXPECT_EQ(c.close, d("2630.2"));
    EXPECT_GE(c.high, c.open);
    EXPECT_LE(c.low, c.open);
    EXPECT_GT(c.volume, Decimal{});
    EXPECT_GT(c.trades, 0U);
}

TEST_F(InfoClientTest, FundingHistoryAndPredictedFundings) {
    response = hltest::fixture("funding_history_eth.json");
    hl::Result<std::vector<hl::FundingRate>> f{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::FundingRate>>(
        [&](auto cb) { info.fundingHistory("ETH", 1789800000000LL, 0, cb); }, f));
    ASSERT_TRUE(f) << f.error().message;
    ASSERT_FALSE(f->empty());
    EXPECT_EQ(lastBody, R"({"type":"fundingHistory","coin":"ETH","startTime":1789800000000})");
    EXPECT_EQ(f->front().coin, "ETH");
    EXPECT_NE(f->front().rate, Decimal{});
    EXPECT_GT(f->front().timeMs, 1700000000000LL);

    response = hltest::fixture("predicted_fundings.json");
    hl::Result<std::vector<hl::PredictedFunding>> p{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::PredictedFunding>>([&](auto cb) { info.predictedFundings(cb); }, p));
    ASSERT_TRUE(p) << p.error().message;
    ASSERT_FALSE(p->empty());
    bool sawHl = false;
    for (const auto& pf : *p) {
        EXPECT_FALSE(pf.coin.empty());
        EXPECT_FALSE(pf.venue.empty());
        EXPECT_GT(pf.nextFundingTimeMs, 1700000000000LL);
        sawHl |= pf.venue == "HlPerp";
    }
    EXPECT_TRUE(sawHl);
}

TEST_F(InfoClientTest, RateLimitAndSpotBalances) {
    response = hltest::fixture("user_rate_limit.json");
    hl::Result<hl::RateLimitStatus> rl{hl::Error{}};
    ASSERT_TRUE(call<hl::RateLimitStatus>([&](auto cb) { info.rateLimit(user, cb); }, rl));
    ASSERT_TRUE(rl) << rl.error().message;
    EXPECT_EQ(rl->cumVlm, d("168.41"));
    EXPECT_EQ(rl->requestsUsed, 147U);
    EXPECT_EQ(rl->requestsCap, 10168U);
    EXPECT_EQ(rl->remaining(), 10021U);

    response = hltest::fixture("spot_clearinghouse_state.json");
    hl::Result<std::vector<hl::SpotBalance>> sb{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::SpotBalance>>([&](auto cb) { info.spotBalances(user, cb); }, sb));
    ASSERT_TRUE(sb) << sb.error().message;
    ASSERT_EQ(sb->size(), 2U);
    EXPECT_EQ((*sb)[0].coin, "USDC");
    EXPECT_EQ((*sb)[0].total, d("1234.5678"));
    EXPECT_EQ((*sb)[0].hold, d("12"));
    EXPECT_EQ((*sb)[1].token, 1U);
    EXPECT_EQ((*sb)[1].entryNtl, d("250.5"));
}

TEST_F(InfoClientTest, UserFundingAndHistoricalOrders) {
    response = hltest::fixture("user_funding.json");
    hl::Result<std::vector<hl::FundingPayment>> fp{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::FundingPayment>>(
        [&](auto cb) { info.userFunding(user, 1789800000000LL, 1789900000000LL, cb); }, fp));
    ASSERT_TRUE(fp) << fp.error().message;
    ASSERT_EQ(fp->size(), 1U);
    EXPECT_EQ(fp->front().coin, "ETH");
    EXPECT_EQ(fp->front().usdc, d("-0.0123"));
    EXPECT_EQ(fp->front().szi, d("1.5"));
    EXPECT_EQ(fp->front().rate, d("0.0000125"));
    EXPECT_NE(lastBody.find(R"("endTime":1789900000000)"), std::string::npos);

    response = hltest::fixture("historical_orders.json");
    hl::Result<std::vector<hl::OrderStatusInfo>> ho{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::OrderStatusInfo>>([&](auto cb) { info.historicalOrders(user, cb); }, ho));
    ASSERT_TRUE(ho) << ho.error().message;
    ASSERT_EQ(ho->size(), 1U);
    EXPECT_TRUE(ho->front().found);
    EXPECT_EQ(ho->front().status, hl::OrderUpdateStatus::Filled);
    EXPECT_EQ(ho->front().order.coin, "ETH");
    EXPECT_EQ(ho->front().order.oid, 123U);
    EXPECT_EQ(ho->front().order.tif, "Alo");
}

TEST_F(InfoClientTest, UserFillsByTimeUsesTheTimeRange) {
    response = R"([{"coin":"BTC","px":"75930.0","sz":"0.3","side":"A","time":1789582997706,"startPosition":"0.3",
                    "dir":"Close Long","closedPnl":"-1.5","hash":"0xabc","oid":1,"crossed":true,"fee":"0.01",
                    "tid":2,"feeToken":"USDC"}])";
    hl::Result<std::vector<hl::Fill>> r{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::Fill>>(
        [&](auto cb) { info.userFillsByTime(user, 1789500000000LL, 0, cb); }, r));
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_EQ(r->size(), 1U);
    EXPECT_EQ(r->front().side, hl::Side::Sell);
    EXPECT_TRUE(r->front().crossed);
    EXPECT_EQ(lastBody, R"({"type":"userFillsByTime","user":"0x14791697260e4c9a71f18484c9f997b308e59325","startTime":1789500000000})");
}

TEST_F(InfoClientTest, UserRoleReportsAgentMaster) {
    response = R"({"role":"agent","data":{"user":"0x1111111111111111111111111111111111111111"}})";
    hl::Result<hl::UserRole> r{hl::Error{}};
    ASSERT_TRUE(call<hl::UserRole>([&](auto cb) { info.userRole(user, cb); }, r));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r->isAgent());
    ASSERT_TRUE(r->master);
    EXPECT_EQ(hl::toHex(*r->master), "0x1111111111111111111111111111111111111111");

    response = R"({"role":"user"})";
    hl::Result<hl::UserRole> plain{hl::Error{}};
    ASSERT_TRUE(call<hl::UserRole>([&](auto cb) { info.userRole(user, cb); }, plain));
    ASSERT_TRUE(plain);
    EXPECT_FALSE(plain->isAgent());
    EXPECT_FALSE(plain->master);
}

TEST_F(InfoClientTest, ErrorsArePropagated) {
    status = 500;
    response = "internal error";
    hl::Result<hl::RateLimitStatus> rl{hl::Error{}};
    ASSERT_TRUE(call<hl::RateLimitStatus>([&](auto cb) { info.rateLimit(user, cb); }, rl));
    ASSERT_FALSE(rl);
    EXPECT_EQ(rl.error().kind, hl::Error::Kind::Http);
    EXPECT_NE(rl.error().message.find("internal error"), std::string::npos);

    status = 200;
    response = "not json";
    hl::Result<std::vector<hl::Candle>> c{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::Candle>>([&](auto cb) { info.candles("ETH", "1m", 0, 0, cb); }, c));
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().kind, hl::Error::Kind::Parse);

    response = R"({"unexpected":"shape"})";
    hl::Result<std::vector<hl::FundingRate>> f{hl::Error{}};
    ASSERT_TRUE(call<std::vector<hl::FundingRate>>([&](auto cb) { info.fundingHistory("ETH", 0, 0, cb); }, f));
    ASSERT_FALSE(f);
    EXPECT_EQ(f.error().kind, hl::Error::Kind::Parse);
}
