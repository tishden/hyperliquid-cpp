// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include <gtest/gtest.h>

#include "hl/om/ExchangeResponse.h"

using K = hl::ActionStatus::Kind;

TEST(ExchangeResponse, RestingWithCloid) {
    auto r = hl::parseExchangeResponse(
        R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":77738308,"cloid":"0x00000000000000000011223344556677"}}]}}})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->type, "order");
    ASSERT_EQ(r->statuses.size(), 1U);
    EXPECT_EQ(r->statuses[0].kind, K::Resting);
    EXPECT_EQ(r->statuses[0].oid, 77738308U);
    ASSERT_TRUE(r->statuses[0].cloid);
    EXPECT_EQ(r->statuses[0].cloid->low(), 0x0011223344556677ULL);
}

TEST(ExchangeResponse, Filled) {
    auto r = hl::parseExchangeResponse(
        R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"filled":{"totalSz":"0.5","avgPx":"50000.0","oid":123}}]}}})");
    ASSERT_TRUE(r);
    const auto& s = r->statuses[0];
    EXPECT_EQ(s.kind, K::Filled);
    EXPECT_EQ(s.oid, 123U);
    EXPECT_EQ(s.totalSz, hl::Decimal::parseOrZero("0.5"));
    EXPECT_EQ(s.avgPx, hl::Decimal::fromInt(50000));
}

TEST(ExchangeResponse, PerOrderErrorsAndBatches) {
    auto r = hl::parseExchangeResponse(
        R"({"status":"ok","response":{"type":"order","data":{"statuses":[
            {"resting":{"oid":1}},
            {"error":"Post only order would have immediately matched, bbo was 50000"},
            "waitingForFill", "waitingForTrigger",
            {"filled":{"totalSz":"1","avgPx":"100","oid":2}}]}}})");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->statuses.size(), 5U);
    EXPECT_EQ(r->statuses[0].kind, K::Resting);
    EXPECT_EQ(r->statuses[1].kind, K::Error);
    EXPECT_NE(r->statuses[1].error.find("Post only"), std::string::npos);
    EXPECT_EQ(r->statuses[2].kind, K::WaitingForFill);
    EXPECT_EQ(r->statuses[3].kind, K::WaitingForTrigger);
    EXPECT_EQ(r->statuses[4].kind, K::Filled);
}

TEST(ExchangeResponse, Cancel) {
    auto ok = hl::parseExchangeResponse(R"({"status":"ok","response":{"type":"cancel","data":{"statuses":["success"]}}})");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->statuses[0].kind, K::Success);
    auto nf = hl::parseExchangeResponse(
        R"({"status":"ok","response":{"type":"cancel","data":{"statuses":[{"error":"Order was never placed, already canceled, or filled."}]}}})");
    ASSERT_TRUE(nf);
    EXPECT_EQ(nf->statuses[0].kind, K::Error);
}

TEST(ExchangeResponse, DefaultTypeHasNoStatuses) {
    auto r = hl::parseExchangeResponse(R"({"status":"ok","response":{"type":"default"}})");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->type, "default");
    EXPECT_TRUE(r->statuses.empty());
}

TEST(ExchangeResponse, TopLevelErrorAndGarbage) {
    auto err = hl::parseExchangeResponse(R"({"status":"err","response":"User or API Wallet 0xabc does not exist."})");
    ASSERT_FALSE(err);
    EXPECT_EQ(err.error().kind, hl::Error::Kind::Venue);
    EXPECT_NE(err.error().message.find("does not exist"), std::string::npos);
    auto bad = hl::parseExchangeResponse("Failed to deserialize the JSON body");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().kind, hl::Error::Kind::Parse);
}
