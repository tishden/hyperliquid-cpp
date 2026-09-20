// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
// End-to-end tests of ExchangeClient against an in-process MockVenue (real sockets,
// real HTTP/WebSocket stack, scripted venue responses).
#include <gtest/gtest.h>

#include <functional>

#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "hl/core/Log.h"
#include "hl/om/ExchangeClient.h"
#include "support/Fixtures.h"
#include "support/MockVenue.h"

using hl::Decimal;
using hl::OrderState;
using hltest::runUntil;

namespace {

constexpr const char* kKey = "0x0123456789012345678901234567890123456789012345678901234567890123";
constexpr const char* kUser = "0x14791697260e4c9a71f18484c9f997b308e59325";

Decimal d(std::string_view s) { return Decimal::parseOrZero(s); }

std::string capture(const std::string& text, const std::string& pattern) {
    std::smatch m;
    return std::regex_search(text, m, std::regex{pattern}) ? m[1].str() : std::string{};
}

// Scripted venue: answers info requests, subscription requests and action posts.
struct FakeHyperliquid {
    hltest::MockVenue venue;
    bool autoRespond{true};
    std::uint64_t nextOid{1000};
    std::map<std::string, std::string> orderStatusByKey;  // cloid or oid text → info JSON
    std::string userRoleResponse{R"({"role":"user"})"};
    std::string openOrdersResponse{"[]"};
    std::string rateLimitResponse{R"({"cumVlm":"0.0","nRequestsUsed":0,"nRequestsCap":10000})"};
    bool serveSpot{false};
    std::string wsInfoResponse{R"({"status":"order"})"};
    std::function<std::string(const std::string& actionType, const std::string& request)> actionPayload;
    std::vector<std::string> posts;
    int wsConn{-1};

    explicit FakeHyperliquid(hl::EventLoop& loop) : venue(loop) {
        venue.onHttp = [this](const std::string& path, const std::string& body) -> hltest::MockVenue::HttpReply {
            if (path == "/info") {
                return {200, info(body)};
            }
            if (path == "/exchange") {
                posts.push_back(body);
                return {200, respond(body)};
            }
            return {404, "no"};
        };
        venue.onWsOpen = [this](int conn) { wsConn = conn; };
        venue.onWsText = [this](int conn, const std::string& text) {
            if (text.find(R"("method":"subscribe")") != std::string::npos) {
                const std::string type = capture(text, R"re("type":"([A-Za-z]+)")re");
                venue.wsSend(conn, R"({"channel":"subscriptionResponse","data":{"method":"subscribe","subscription":{"type":")" +
                                       type + R"(","user":")" + kUser + R"("}}})");
                if (type == "userFills") {
                    venue.wsSend(conn, std::string{R"({"channel":"userFills","data":{"isSnapshot":true,"user":")"} + kUser +
                                           R"(","fills":[)" + fillJson("BTC", "B", "1", "0.5", "0", 1, 1, "") + "]}}");
                }
            } else if (text.find(R"("request":{"type":"info")") != std::string::npos) {
                const std::string id = capture(text, R"re("id":([0-9]+))re");
                venue.wsSend(conn, R"({"channel":"post","data":{"id":)" + id +
                                       R"(,"response":{"type":"info","payload":)" + wsInfoResponse + "}}}");
            } else if (text.find(R"("method":"post")") != std::string::npos) {
                posts.push_back(text);
                if (autoRespond) {
                    const std::string id = capture(text, R"re("id":([0-9]+))re");
                    venue.wsSend(conn, R"({"channel":"post","data":{"id":)" + id +
                                           R"(,"response":{"type":"action","payload":)" + respond(text) + "}}}");
                }
            } else if (text.find(R"("method":"ping")") != std::string::npos) {
                venue.wsSend(conn, R"({"channel":"pong"})");
            }
        };
    }

    std::string info(const std::string& body) {
        if (body.find(R"("type":"meta")") != std::string::npos) {
            return hltest::fixture("meta_testnet.json");
        }
        // "spotClearinghouseState" contains "clearinghouseState", so the spot routes come first.
        if (serveSpot && body.find("spotClearinghouseState") != std::string::npos) {
            return hltest::fixture("spot_clearinghouse_state.json");
        }
        if (serveSpot && body.find(R"("type":"spotMeta")") != std::string::npos) {
            return hltest::fixture("spot_meta_testnet.json");
        }
        if (body.find("clearinghouseState") != std::string::npos) {
            return R"({"marginSummary":{"accountValue":"1000.5","totalNtlPos":"1500","totalRawUsd":"2500","totalMarginUsed":"75"},)"
                   R"("crossMarginSummary":{},"withdrawable":"900","assetPositions":[{"type":"oneWay","position":{"coin":"ETH",)"
                   R"("szi":"-0.5","entryPx":"3000","positionValue":"1500","unrealizedPnl":"-2","returnOnEquity":"0",)"
                   R"("liquidationPx":null,"marginUsed":"75","leverage":{"type":"cross","value":20}}}],"time":1})";
        }
        if (body.find("userRole") != std::string::npos) {
            return userRoleResponse;
        }
        if (body.find("frontendOpenOrders") != std::string::npos) {
            return openOrdersResponse;
        }
        if (body.find("userRateLimit") != std::string::npos) {
            return rateLimitResponse;
        }
        if (body.find("orderStatus") != std::string::npos) {
            std::string key = capture(body, R"re("oid":"?(0x[0-9a-f]+|[0-9]+)"?)re");
            auto it = orderStatusByKey.find(key);
            return it == orderStatusByKey.end() ? R"({"status":"unknownOid"})" : it->second;
        }
        return "[]";
    }

    // Default: every order rests with a fresh oid, cancels/modifies succeed.
    std::string respond(const std::string& request) {
        const std::string type = capture(request, R"re("action":\{"type":"([A-Za-z]+)")re");
        if (actionPayload) {
            std::string custom = actionPayload(type, request);
            if (!custom.empty()) {
                return custom;
            }
        }
        if (type == "order" || type == "batchModify") {
            std::string statuses;
            std::size_t pos = 0;
            const std::string needle = type == "order" ? R"({"a":)" : R"({"oid":)";
            while ((pos = request.find(needle, pos)) != std::string::npos) {
                statuses += (statuses.empty() ? "" : ",") + std::string{R"({"resting":{"oid":)"} + std::to_string(nextOid++) + "}}";
                pos += needle.size();
            }
            return R"({"status":"ok","response":{"type":")" + type + R"(","data":{"statuses":[)" + statuses + "]}}}";
        }
        if (type == "cancel" || type == "cancelByCloid") {
            std::size_t n = 0;
            std::size_t pos = 0;
            const std::string needle = type == "cancel" ? R"("o":)" : R"("cloid":)";
            while ((pos = request.find(needle, pos)) != std::string::npos) {
                ++n;
                pos += needle.size();
            }
            std::string statuses;
            for (std::size_t i = 0; i < n; ++i) {
                statuses += (i ? "," : "") + std::string{R"("success")"};
            }
            return R"({"status":"ok","response":{"type":"cancel","data":{"statuses":[)" + statuses + "]}}}";
        }
        return R"({"status":"ok","response":{"type":"default"}})";
    }

    static std::string fillJson(const std::string& coin, const std::string& side, const std::string& sz,
                                const std::string& px, const std::string& startPos, std::uint64_t tid, std::uint64_t oid,
                                const std::string& cloid) {
        std::string j = R"({"coin":")" + coin + R"(","px":")" + px + R"(","sz":")" + sz + R"(","side":")" + side +
                        R"(","time":1700000000000,"startPosition":")" + startPos +
                        R"(","dir":"Open Long","closedPnl":"0.0","hash":"0xabc","oid":)" + std::to_string(oid) +
                        R"(,"crossed":false,"fee":"-0.001","tid":)" + std::to_string(tid) + R"(,"feeToken":"USDC")";
        if (!cloid.empty()) {
            j += R"(,"cloid":")" + cloid + '"';
        }
        return j + "}";
    }

    void pushFill(const std::string& fill) {
        venue.wsBroadcast(std::string{R"({"channel":"userFills","data":{"isSnapshot":false,"user":")"} + kUser +
                          R"(","fills":[)" + fill + "]}}");
    }

    void pushOrderUpdate(const std::string& coin, const std::string& side, const std::string& px, const std::string& sz,
                         const std::string& origSz, std::uint64_t oid, const std::string& cloid,
                         const std::string& status) {
        std::string order = R"({"coin":")" + coin + R"(","side":")" + side + R"(","limitPx":")" + px + R"(","sz":")" + sz +
                            R"(","oid":)" + std::to_string(oid) + R"(,"timestamp":1700000000000,"origSz":")" + origSz + '"';
        if (!cloid.empty()) {
            order += R"(,"cloid":")" + cloid + '"';
        }
        order += "}";
        venue.wsBroadcast(R"({"channel":"orderUpdates","data":[{"order":)" + order + R"(,"status":")" + status +
                          R"(","statusTimestamp":1700000000001}]})");
    }
};

struct Listener : hl::ExchangeListener {
    int ready{0};
    /// Set from inside onReady() so a test can assert what the client knew at that instant.
    std::function<void()> onReadyHook;
    int disconnects{0};
    std::vector<hl::Order> updates;
    std::vector<hl::Fill> fills;
    std::vector<hl::Error> errors;
    void onReady() override {
        ++ready;
        if (onReadyHook) {
            onReadyHook();
        }
    }
    void onDisconnected(std::string_view) override { ++disconnects; }
    void onOrderUpdate(const hl::Order& o) override { updates.push_back(o); }
    void onFill(const hl::Fill& f) override { fills.push_back(f); }
    void onError(const hl::Error& e) override { errors.push_back(e); }
};

class ExchangeClientTest : public ::testing::Test {
protected:
    void SetUp() override { hl::setLogLevel(hl::LogLevel::Error); }

    std::unique_ptr<hl::ExchangeClient> makeClient(hl::ActionTransport transport = hl::ActionTransport::WebSocket,
                                                   std::int64_t timeoutMs = 2000, bool loadSpot = false) {
        hl::ExchangeConfig cfg;
        cfg.loadSpotAssets = loadSpot;
        cfg.network = hl::Network::Testnet;
        cfg.privateKey = kKey;
        cfg.transport = transport;
        cfg.requestTimeoutMs = timeoutMs;
        cfg.restUrlOverride = fake.venue.httpUrl();
        cfg.wsUrlOverride = fake.venue.wsUrl();
        cfg.session.reconnectMinDelayMs = 20;
        cfg.session.reconnectMaxDelayMs = 50;
        return std::make_unique<hl::ExchangeClient>(loop, listener, cfg);
    }

    std::unique_ptr<hl::ExchangeClient> startReady(hl::ActionTransport transport = hl::ActionTransport::WebSocket,
                                                   std::int64_t timeoutMs = 2000) {
        auto c = makeClient(transport, timeoutMs);
        c->start();
        EXPECT_TRUE(runUntil(loop, [&] { return listener.ready > 0; }));
        return c;
    }

    static hl::OrderRequest btcBuy(std::string_view px = "50000", std::string_view sz = "0.002") {
        hl::OrderRequest r;
        r.coin = "BTC";
        r.side = hl::Side::Buy;
        r.px = d(px);
        r.sz = d(sz);
        r.tif = hl::Tif::Alo;
        return r;
    }

    hl::ExchangeClient* clientUnderTest{nullptr};
    hl::EventLoop loop;
    FakeHyperliquid fake{loop};
    Listener listener;
};

}  // namespace

TEST_F(ExchangeClientTest, BootstrapsToReady) {
    auto client = startReady();
    EXPECT_TRUE(client->isReady());
    EXPECT_EQ(hl::toHex(client->accountAddress()), kUser);
    EXPECT_EQ(client->signerAddress(), client->accountAddress());
    ASSERT_NE(client->assets().find("BTC"), nullptr);
    EXPECT_EQ(client->position("ETH"), d("-0.5"));
    EXPECT_EQ(client->position("BTC"), Decimal{});  // snapshot fills are historical
    EXPECT_TRUE(listener.fills.empty());
    EXPECT_TRUE(listener.errors.empty());
}

TEST_F(ExchangeClientTest, ValidatesLocally) {
    auto notStarted = makeClient();
    EXPECT_FALSE(notStarted->placeOrder(btcBuy()));

    auto client = startReady();
    auto unknown = btcBuy();
    unknown.coin = "DOESNOTEXIST";
    EXPECT_EQ(client->placeOrder(unknown).error().kind, hl::Error::Kind::Rejected);
    auto r1 = client->placeOrder(btcBuy("50000.5"));  // BTC: max 1 decimal but 5 sig figs → 50000 or 50001
    ASSERT_FALSE(r1);
    EXPECT_NE(r1.error().message.find("invalid price"), std::string::npos);
    EXPECT_FALSE(client->placeOrder(btcBuy("50000", "0.0000001")));
    EXPECT_FALSE(client->placeOrder(btcBuy("0", "0.001")));
    EXPECT_TRUE(fake.posts.empty());
}

TEST_F(ExchangeClientTest, OrderLifecycleRestingPartialFilled) {
    auto client = startReady();
    auto placed = client->placeOrder(btcBuy());
    ASSERT_TRUE(placed) << placed.error().message;
    const hl::Cloid cloid = placed.value();
    ASSERT_NE(client->findOrder(cloid), nullptr);
    EXPECT_EQ(client->findOrder(cloid)->state, OrderState::PendingNew);

    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    ASSERT_EQ(fake.posts.size(), 1U);
    const std::string& post = fake.posts[0];
    EXPECT_NE(post.find(R"("type":"order")"), std::string::npos);
    EXPECT_NE(post.find(R"("p":"50000","s":"0.002")"), std::string::npos);
    EXPECT_NE(post.find(R"("tif":"Alo")"), std::string::npos);
    EXPECT_NE(post.find(cloid.toString()), std::string::npos);
    EXPECT_NE(post.find(R"("signature":{"r":"0x)"), std::string::npos);
    const std::uint64_t oid = client->findOrder(cloid)->oid;
    EXPECT_EQ(oid, 1000U);

    const std::string fill1 = FakeHyperliquid::fillJson("BTC", "B", "0.001", "50000", "0", 10, oid, cloid.toString());
    fake.pushFill(fill1);
    fake.pushFill(fill1);  // duplicate tid must be ignored
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::PartiallyFilled; }));
    EXPECT_EQ(client->findOrder(cloid)->filledSz, d("0.001"));
    EXPECT_EQ(client->position("BTC"), d("0.001"));

    fake.pushOrderUpdate("BTC", "B", "50000", "0", "0.002", oid, cloid.toString(), "filled");
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Filled; }));
    EXPECT_EQ(client->findOrder(cloid)->filledSz, d("0.002"));

    fake.pushFill(FakeHyperliquid::fillJson("BTC", "B", "0.001", "50001", "0.001", 11, oid, cloid.toString()));
    ASSERT_TRUE(runUntil(loop, [&] { return listener.fills.size() == 2; }));
    const hl::Order* o = client->findOrder(cloid);
    EXPECT_EQ(o->filledSz, d("0.002"));  // not double counted
    EXPECT_EQ(o->avgFillPx, d("50000.5"));
    EXPECT_EQ(client->position("BTC"), d("0.002"));
    EXPECT_TRUE(client->liveOrders().empty());
    EXPECT_EQ(listener.fills[0].tid, 10U);
    EXPECT_EQ(client->stats().fills, 2U);
}

TEST_F(ExchangeClientTest, PerOrderRejection) {
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        if (type == "order") {
            return R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"error":"Post only order would have immediately matched, bbo was 50001"}]}}})";
        }
        return {};
    };
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Rejected; }));
    EXPECT_NE(client->findOrder(cloid)->lastError.find("Post only"), std::string::npos);
}

TEST_F(ExchangeClientTest, BatchPlacementMapsStatusesInOrder) {
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        if (type == "order") {
            return R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":1}},{"error":"Insufficient margin"}]}}})";
        }
        return {};
    };
    auto client = startReady();
    std::vector<hl::OrderRequest> reqs{btcBuy("50000"), btcBuy("49000")};
    auto placed = client->placeOrders(reqs);
    ASSERT_TRUE(placed);
    ASSERT_EQ(placed->size(), 2U);
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder((*placed)[1])->state == OrderState::Rejected; }));
    EXPECT_EQ(client->findOrder((*placed)[0])->state, OrderState::Open);
    EXPECT_EQ(fake.posts.size(), 1U);
}

TEST_F(ExchangeClientTest, CancelByCloid) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    EXPECT_FALSE(client->cancel(cloid));
    EXPECT_TRUE(client->findOrder(cloid)->cancelPending);
    EXPECT_TRUE(client->cancel(cloid));  // already pending
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Canceled; }));
    EXPECT_NE(fake.posts.back().find(R"("type":"cancelByCloid")"), std::string::npos);
    EXPECT_FALSE(client->findOrder(cloid)->cancelPending);
    EXPECT_EQ(client->cancel(cloid).kind, hl::Error::Kind::Rejected);
}

TEST_F(ExchangeClientTest, FailedCancelIsReconciled) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        if (type == "cancelByCloid") {
            return R"({"status":"ok","response":{"type":"cancel","data":{"statuses":[{"error":"Order was never placed, already canceled, or filled."}]}}})";
        }
        return {};
    };
    // An acknowledged order is probed by its oid — a cloid probe answers about the first
    // generation of the order, which after an amendment is no longer the live one.
    fake.orderStatusByKey[std::to_string(client->findOrder(cloid)->oid)] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.0","oid":1000,"timestamp":1,)"
        R"("origSz":"0.002","cloid":")" + cloid.toString() + R"("},"status":"filled","statusTimestamp":2}})";
    ASSERT_FALSE(client->cancel(cloid));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Filled; }));
    EXPECT_EQ(client->findOrder(cloid)->filledSz, d("0.002"));
    EXPECT_GE(client->stats().reconciles, 1U);
}

TEST_F(ExchangeClientTest, ModifyKeepsCloidAndIgnoresStaleCancel) {
    fake.autoRespond = false;
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return fake.posts.size() == 1; }));
    auto respondLast = [&](const std::string& payload) {
        const std::string id = capture(fake.posts.back(), R"re("id":([0-9]+))re");
        fake.venue.wsSend(fake.wsConn, R"({"channel":"post","data":{"id":)" + id +
                                           R"(,"response":{"type":"action","payload":)" + payload + "}}}");
    };
    respondLast(R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":1}}]}}})");
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->oid == 1; }));

    ASSERT_FALSE(client->modify(cloid, d("49000"), d("0.003")));
    ASSERT_TRUE(runUntil(loop, [&] { return fake.posts.size() == 2; }));
    EXPECT_NE(fake.posts[1].find(R"("type":"batchModify","modifies":[{"oid":1,)"), std::string::npos);
    EXPECT_NE(fake.posts[1].find(cloid.toString()), std::string::npos);
    EXPECT_TRUE(client->modify(cloid, d("48000"), d("0.003")));  // modify already pending

    // Venue cancels the old oid and opens the replacement before the ack arrives.
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", 1, cloid.toString(), "canceled");
    fake.pushOrderUpdate("BTC", "B", "49000", "0.003", "0.003", 2, cloid.toString(), "open");
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->oid == 2; }));
    respondLast(R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":2}}]}}})");
    ASSERT_TRUE(runUntil(loop, [&] { return !client->findOrder(cloid)->modifyPending; }));
    const hl::Order* o = client->findOrder(cloid);
    EXPECT_EQ(o->state, OrderState::Open);
    EXPECT_EQ(o->px, d("49000"));
    EXPECT_EQ(o->origSz, d("0.003"));

    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", 1, cloid.toString(), "canceled");  // stale again
    loop.runOnce(50);
    EXPECT_EQ(client->findOrder(cloid)->state, OrderState::Open);
}

TEST_F(ExchangeClientTest, TerminalStatesAreSticky) {
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        return type == "order"
                   ? R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"error":"Insufficient margin"}]}}})"
                   : "";
    };
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Rejected; }));
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", 0, cloid.toString(), "canceled");
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", 0, cloid.toString(), "open");
    loop.runOnce(50);
    loop.runOnce(50);
    EXPECT_EQ(client->findOrder(cloid)->state, OrderState::Rejected);
    EXPECT_EQ(client->findOrder(cloid)->lastError, "Insufficient margin");
}

TEST_F(ExchangeClientTest, TimeoutTriggersOrderStatusReconcile) {
    fake.autoRespond = false;
    auto client = startReady(hl::ActionTransport::WebSocket, 150);
    const auto cloid = client->placeOrder(btcBuy()).value();
    fake.orderStatusByKey[cloid.toString()] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.002","oid":77,"timestamp":1,)"
        R"("origSz":"0.002","cloid":")" + cloid.toString() + R"("},"status":"open","statusTimestamp":2}})";
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    EXPECT_EQ(client->findOrder(cloid)->oid, 77U);
    EXPECT_EQ(client->stats().timeouts, 1U);
}

TEST_F(ExchangeClientTest, UnknownAfterTimeoutBecomesRejected) {
    fake.autoRespond = false;
    auto client = startReady(hl::ActionTransport::WebSocket, 100);
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Rejected; }));
}

TEST_F(ExchangeClientTest, HttpTransport) {
    auto client = startReady(hl::ActionTransport::Http);
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    EXPECT_EQ(client->stats().actionsViaHttp, 1U);
    bool scheduled = false;
    ASSERT_FALSE(client->scheduleCancel(hl::EventLoop::wallClockMs() + 60'000, [&](const auto& r) {
        EXPECT_TRUE(r.ok());
        EXPECT_EQ(r->type, "default");
        scheduled = true;
    }));
    ASSERT_TRUE(runUntil(loop, [&] { return scheduled; }));
    bool leverage = false;
    ASSERT_FALSE(client->updateLeverage("BTC", 5, true, [&](const auto& r) { leverage = r.ok(); }));
    ASSERT_TRUE(runUntil(loop, [&] { return leverage; }));
    EXPECT_NE(fake.posts.back().find(R"("type":"updateLeverage")"), std::string::npos);
    EXPECT_TRUE(client->updateLeverage("BTC", 1000, true));  // above maxLeverage
}

TEST_F(ExchangeClientTest, VenueErrorOnAction) {
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        return type == "order" ? R"({"status":"err","response":"User or API Wallet does not exist."})" : "";
    };
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Rejected; }));
    EXPECT_NE(client->findOrder(cloid)->lastError.find("does not exist"), std::string::npos);
    EXPECT_EQ(client->stats().actionErrors, 1U);
}

TEST_F(ExchangeClientTest, ReconnectReconcilesLiveOrders) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));

    fake.orderStatusByKey[std::to_string(client->findOrder(cloid)->oid)] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.002","oid":1000,"timestamp":1,)"
        R"("origSz":"0.002","cloid":")" + cloid.toString() + R"("},"status":"marginCanceled","statusTimestamp":2}})";
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.disconnects == 1; }));
    EXPECT_FALSE(client->isReady());
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Canceled; }));
    EXPECT_EQ(client->findOrder(cloid)->lastError, "marginCanceled");
}

// The venue resolves a cloid to the *first* order placed with it, while `modify` carries the cloid
// over to a new oid: after an amendment an orderStatus-by-cloid probe answers "canceled" about the
// superseded generation while the live order rests. Reconciliation must not believe it — that marks
// a working order terminal and leaves it on the venue with nothing tracking it.
TEST_F(ExchangeClientTest, ReconcileAfterAmendmentProbesByOidNotCloid) {
    auto statusJson = [](std::uint64_t oid, const std::string& px, const std::string& status,
                         const std::string& cloid) {
        return R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":")" + px +
               R"(","sz":"0.002","oid":)" + std::to_string(oid) + R"(,"timestamp":1,"origSz":"0.002","cloid":")" +
               cloid + R"("},"status":")" + status + R"(","statusTimestamp":2}})";
    };
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t firstOid = client->findOrder(cloid)->oid;

    ASSERT_FALSE(client->modify(cloid, d("49000"), d("0.002")));
    ASSERT_TRUE(runUntil(loop, [&] { return !client->findOrder(cloid)->modifyPending; }));
    const std::uint64_t liveOid = client->findOrder(cloid)->oid;
    ASSERT_NE(liveOid, firstOid);

    fake.orderStatusByKey[cloid.toString()] = statusJson(firstOid, "50000.0", "canceled", cloid.toString());
    fake.orderStatusByKey[std::to_string(liveOid)] = statusJson(liveOid, "49000.0", "open", cloid.toString());

    // A second amendment is lost with the socket, so its outcome is unknown and gets reconciled.
    fake.autoRespond = false;
    ASSERT_FALSE(client->modify(cloid, d("48000"), d("0.002")));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->modifyPending; }));
    fake.autoRespond = true;
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    ASSERT_TRUE(runUntil(loop, [&] { return !client->findOrder(cloid)->modifyPending; }));

    const hl::Order* o = client->findOrder(cloid);
    ASSERT_NE(o, nullptr);
    EXPECT_TRUE(o->isLive()) << "the order rests on the venue; state is " << hl::toString(o->state);
    EXPECT_EQ(o->oid, liveOid) << "the tracked oid must not fall back to a superseded generation";
    EXPECT_EQ(client->liveOrders("BTC").size(), 1U);
}

TEST_F(ExchangeClientTest, FillsMissedWhileDisconnectedAreApplied) {
    auto client = startReady();
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    // A second snapshot after reconnect contains a fill not seen before → applied.
    fake.venue.wsBroadcast(std::string{R"({"channel":"userFills","data":{"isSnapshot":true,"user":")"} + kUser +
                           R"(","fills":[)" + FakeHyperliquid::fillJson("SOL", "A", "2", "150", "0", 99, 5, "") + "]}}");
    ASSERT_TRUE(runUntil(loop, [&] { return listener.fills.size() == 1; }));
    EXPECT_EQ(client->position("SOL"), d("-2"));
}

TEST_F(ExchangeClientTest, ExternalOrdersAreTrackedAndCancelable) {
    auto client = startReady();
    fake.pushOrderUpdate("ETH", "A", "3000", "1", "1", 555, "", "open");
    ASSERT_TRUE(runUntil(loop, [&] { return client->liveOrders("ETH").size() == 1; }));
    const hl::Order* ext = client->liveOrders("ETH")[0];
    EXPECT_TRUE(ext->external);
    EXPECT_EQ(ext->oid, 555U);
    ASSERT_FALSE(client->cancel(ext->cloid));
    ASSERT_TRUE(runUntil(loop, [&] { return client->liveOrders("ETH").empty(); }));
    EXPECT_NE(fake.posts.back().find(R"("type":"cancel","cancels":[{"a":)"), std::string::npos);
    EXPECT_NE(fake.posts.back().find(R"("o":555)"), std::string::npos);
}

TEST_F(ExchangeClientTest, CancelAllForCoin) {
    auto client = startReady();
    const auto a = client->placeOrder(btcBuy("50000")).value();
    const auto b = client->placeOrder(btcBuy("49000")).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->liveOrders("BTC").size() == 2 &&
                                            client->findOrder(b)->state == OrderState::Open; }));
    ASSERT_FALSE(client->cancelAll("BTC"));
    ASSERT_TRUE(runUntil(loop, [&] { return client->liveOrders().empty(); }));
    EXPECT_EQ(client->findOrder(a)->state, OrderState::Canceled);
    EXPECT_EQ(client->findOrder(b)->state, OrderState::Canceled);
    const std::string& last = fake.posts.back();
    EXPECT_NE(last.find(a.toString()), std::string::npos);
    EXPECT_NE(last.find(b.toString()), std::string::npos);
}

TEST_F(ExchangeClientTest, AgentWalletUsesMasterAccountForStreams) {
    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;
    cfg.accountAddress = "0x00000000000000000000000000000000000000aa";
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    hl::ExchangeClient client(loop, listener, cfg);
    client.start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 1; }));
    EXPECT_NE(client.signerAddress(), client.accountAddress());
    bool subscribedMaster = false;
    for (const auto& m : fake.venue.wsLog) {
        subscribedMaster |= m.find("0x00000000000000000000000000000000000000aa") != std::string::npos;
    }
    EXPECT_TRUE(subscribedMaster);
}

TEST_F(ExchangeClientTest, AgentWalletWithoutAccountAddressAdoptsItsMaster) {
    constexpr const char* kMaster = "0x1111111111111111111111111111111111111111";
    fake.userRoleResponse = std::string{R"({"role":"agent","data":{"user":")"} + kMaster + R"("}})";
    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;  // no accountAddress configured
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    hl::ExchangeClient client(loop, listener, cfg);
    client.start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready > 0 && hl::toHex(client.accountAddress()) == kMaster; }));
    EXPECT_TRUE(listener.errors.empty());
    bool subscribedMaster = false;
    for (const auto& m : fake.venue.wsLog) {
        subscribedMaster |= m.find(R"("type":"userFills")") != std::string::npos && m.find(kMaster) != std::string::npos;
    }
    EXPECT_TRUE(subscribedMaster) << "user streams must follow the master account";
}

TEST_F(ExchangeClientTest, AgentWalletWithWrongAccountAddressIsReported) {
    fake.userRoleResponse = R"({"role":"agent","data":{"user":"0x1111111111111111111111111111111111111111"}})";
    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;
    cfg.accountAddress = "0x00000000000000000000000000000000000000aa";  // not the master
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    hl::ExchangeClient client(loop, listener, cfg);
    client.start();
    ASSERT_TRUE(runUntil(loop, [&] { return !listener.errors.empty(); }));
    EXPECT_NE(listener.errors[0].message.find("does not match the agent's master account"), std::string::npos);
    EXPECT_EQ(hl::toHex(client.accountAddress()), "0x00000000000000000000000000000000000000aa");
}

TEST_F(ExchangeClientTest, RejectsMalformedConfig) {
    hl::ExchangeConfig cfg;
    EXPECT_THROW(hl::ExchangeClient(loop, listener, cfg), std::invalid_argument);
    cfg.privateKey = kKey;
    cfg.vaultAddress = "0x12";
    EXPECT_THROW(hl::ExchangeClient(loop, listener, cfg), std::invalid_argument);
}

// ── regression tests for the behaviours the code review flagged ─────────────

TEST_F(ExchangeClientTest, AgentAdoptionResubscribesEveryUserChannel) {
    constexpr const char* kMaster = "0x1111111111111111111111111111111111111111";
    fake.userRoleResponse = std::string{R"({"role":"agent","data":{"user":")"} + kMaster + R"("}})";
    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    hl::ExchangeClient client(loop, listener, cfg);
    client.start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready > 0 && hl::toHex(client.accountAddress()) == kMaster; }));
    // Every user subscription must be re-sent for the master, and none left on the agent address.
    int masterSubs = 0;
    int agentSubs = 0;
    for (const auto& m : fake.venue.wsLog) {
        if (m.find(R"("method":"subscribe")") == std::string::npos) {
            continue;
        }
        masterSubs += m.find(kMaster) != std::string::npos ? 1 : 0;
        agentSubs += m.find(kUser) != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(masterSubs, 3) << "orderUpdates + userFills + userEvents";
    EXPECT_EQ(agentSubs, 3) << "the initial (wrong) subscriptions, later unsubscribed";
    EXPECT_EQ(client.session().subscriptions().size(), 3U);
}

TEST_F(ExchangeClientTest, StaleFillReplayDoesNotRewindThePosition) {
    auto client = startReady();
    const std::string newer = FakeHyperliquid::fillJson("BTC", "B", "0.002", "50000", "0", 100, 1, "");
    fake.pushFill(newer);
    ASSERT_TRUE(runUntil(loop, [&] { return client->position("BTC") == d("0.002"); }));
    // An old fill (older timestamp, unseen tid) may reappear in a snapshot after a reconnect.
    std::string older = FakeHyperliquid::fillJson("BTC", "B", "0.001", "50000", "0", 101, 1, "");
    const auto timePos = older.find("1700000000000");
    older.replace(timePos, 13, "1600000000000");
    fake.pushFill(older);
    loop.runOnce(50);
    loop.runOnce(50);
    EXPECT_EQ(client->position("BTC"), d("0.002")) << "an older fill must not move the position";
}

TEST_F(ExchangeClientTest, AdoptsOrdersAlreadyOpenOnTheVenue) {
    fake.openOrdersResponse =
        R"([{"coin":"ETH","side":"A","limitPx":"3000.0","sz":"0.5","oid":4242,"timestamp":1700000000000,)"
        R"("origSz":"1.0","reduceOnly":false,"orderType":"Limit","tif":"Gtc","isTrigger":false,"triggerPx":"0.0"}])";
    auto client = startReady();
    ASSERT_TRUE(runUntil(loop, [&] { return !client->liveOrders("ETH").empty(); }));
    const hl::Order* o = client->liveOrders("ETH").front();
    EXPECT_TRUE(o->external);
    EXPECT_EQ(o->oid, 4242U);
    EXPECT_EQ(o->origSz, d("1"));
    EXPECT_EQ(o->filledSz, d("0.5")) << "origSz - remaining";
    EXPECT_EQ(o->state, hl::OrderState::PartiallyFilled);
    // …and it can be canceled like any other order.
    ASSERT_FALSE(client->cancel(o->cloid));
    ASSERT_TRUE(runUntil(loop, [&] { return client->liveOrders("ETH").empty(); }));
}

// One venue rejection reaches the client three ways — the orderUpdates message, the acknowledgement
// of the action that caused it, and the reconciliation answer. onOrderUpdate reports changes, not
// messages, so the strategy must hear about it once: a quoter that counts rejections (and backs off
// on them) would otherwise triple-count a single one.
TEST_F(ExchangeClientTest, OneRejectionIsReportedOnce) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t oid = client->findOrder(cloid)->oid;
    listener.updates.clear();

    // A post-only amendment that would cross: the venue rejects the new order and the answer comes
    // back on every channel at once.
    fake.orderStatusByKey[std::to_string(oid)] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.002","oid":)" +
        std::to_string(oid) + R"(,"timestamp":1,"origSz":"0.002","cloid":")" + cloid.toString() +
        R"("},"status":"badAloPxRejected","statusTimestamp":2}})";
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        if (type == "batchModify") {
            return R"({"status":"ok","response":{"type":"batchModify","data":{"statuses":[{"error":"badAloPxRejected"}]}}})";
        }
        return {};
    };
    ASSERT_FALSE(client->modify(cloid, d("50500"), d("0.002")));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Rejected; }));
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", oid, cloid.toString(), "badAloPxRejected");
    loop.runOnce(50);
    loop.runOnce(50);

    const auto rejected = std::count_if(listener.updates.begin(), listener.updates.end(),
                                        [](const hl::Order& o) { return o.state == OrderState::Rejected; });
    EXPECT_EQ(rejected, 1) << "one rejection, one callback";
}

// A quoter that amends once a second outruns any bounded memory of retired oids: the venue's
// `canceled` for the generation an amendment replaced can arrive after several further amendments.
// Such an update must not bury the order that is actually resting — this leaked a live quote onto
// testnet roughly once an hour.
TEST_F(ExchangeClientTest, LateCancelOfAnOldGenerationDoesNotKillTheLiveOrder) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t firstOid = client->findOrder(cloid)->oid;

    for (int i = 1; i <= 6; ++i) {  // more amendments than the retired-oid list can remember
        ASSERT_FALSE(client->modify(cloid, d("50000") - d("1").mul(Decimal::fromInt(i)), d("0.002")));
        ASSERT_TRUE(runUntil(loop, [&] { return !client->findOrder(cloid)->modifyPending; }));
    }
    const std::uint64_t liveOid = client->findOrder(cloid)->oid;
    ASSERT_GT(liveOid, firstOid);

    // The venue reports, late, that the very first generation was canceled by the first amendment.
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", firstOid, cloid.toString(), "canceled");
    loop.runOnce(50);
    loop.runOnce(50);

    const hl::Order* o = client->findOrder(cloid);
    ASSERT_NE(o, nullptr);
    EXPECT_TRUE(o->isLive()) << "the amended order rests on the venue; state is " << hl::toString(o->state);
    EXPECT_EQ(o->oid, liveOid) << "a superseded oid must not become the tracked one";
}

// The other half of the amendment problem. A modify whose acknowledgement is lost may still have
// been applied: the venue cancels the oid the client knows and opens a new one under the same
// cloid. Probing that oid answers "canceled" — believing it buries an order that is resting, which
// is how the testnet soak leaked a quote. Only the open-orders listing identifies the successor.
TEST_F(ExchangeClientTest, ModifyAppliedButUnacknowledgedKeepsTheOrderUnderItsNewOid) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t oldOid = client->findOrder(cloid)->oid;
    const std::uint64_t newOid = oldOid + 7;

    // The venue applied the amendment; the old generation is canceled and the new one rests.
    fake.orderStatusByKey[std::to_string(oldOid)] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.002","oid":)" +
        std::to_string(oldOid) + R"(,"timestamp":1,"origSz":"0.002","cloid":")" + cloid.toString() +
        R"("},"status":"canceled","statusTimestamp":2}})";
    fake.openOrdersResponse =
        R"([{"coin":"BTC","side":"B","limitPx":"49000.0","sz":"0.002","oid":)" + std::to_string(newOid) +
        R"(,"timestamp":1700000000000,"origSz":"0.002","cloid":")" + cloid.toString() +
        R"(","reduceOnly":false,"orderType":"Limit","tif":"Alo","isTrigger":false,"triggerPx":"0.0"}])";

    // The acknowledgement never comes: the socket dies with the modify in flight.
    fake.autoRespond = false;
    ASSERT_FALSE(client->modify(cloid, d("49000"), d("0.002")));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->modifyPending; }));
    fake.autoRespond = true;
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->oid == newOid; }));

    const hl::Order* o = client->findOrder(cloid);
    ASSERT_NE(o, nullptr);
    EXPECT_TRUE(o->isLive()) << "the amended order rests on the venue; state is " << hl::toString(o->state);
    EXPECT_EQ(o->px, d("49000"));
    EXPECT_EQ(client->liveOrders("BTC").size(), 1U);
}

// Belt and braces for the same class of mistake: whatever the table believes, an order the venue
// lists as open under a different oid is alive and must be managed again.
TEST_F(ExchangeClientTest, AnOrderBuriedInTheTableIsRevivedByTheOpenOrdersListing) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t oid = client->findOrder(cloid)->oid;
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", oid, cloid.toString(), "canceled");
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Canceled; }));

    fake.openOrdersResponse =
        R"([{"coin":"BTC","side":"B","limitPx":"49000.0","sz":"0.002","oid":)" + std::to_string(oid + 11) +
        R"(,"timestamp":1700000000000,"origSz":"0.002","cloid":")" + cloid.toString() +
        R"(","reduceOnly":false,"orderType":"Limit","tif":"Alo","isTrigger":false,"triggerPx":"0.0"}])";
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->isLive(); }));
    EXPECT_EQ(client->findOrder(cloid)->oid, oid + 11);
    EXPECT_EQ(client->liveOrders("BTC").size(), 1U);
}

// A reconnect is the moment an order can slip out of the table: the acknowledgement that would
// have named its oid went down with the socket. Whatever the venue still reports open and the
// client cannot account for must be adopted, not left resting unmanaged.
TEST_F(ExchangeClientTest, OrdersTheClientLostTrackOfAreAdoptedOnReconnect) {
    auto client = startReady();
    ASSERT_TRUE(client->liveOrders("ETH").empty());
    fake.openOrdersResponse =
        R"([{"coin":"ETH","side":"A","limitPx":"3000.0","sz":"1.0","oid":4242,"timestamp":1700000000000,)"
        R"("origSz":"1.0","reduceOnly":false,"orderType":"Limit","tif":"Gtc","isTrigger":false,"triggerPx":"0.0"}])";
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    ASSERT_TRUE(runUntil(loop, [&] { return !client->liveOrders("ETH").empty(); }));
    const hl::Order* o = client->liveOrders("ETH").front();
    EXPECT_EQ(o->oid, 4242U);
    EXPECT_TRUE(o->external);
    ASSERT_FALSE(client->cancel(o->cloid)) << "an adopted order must be cancelable";
}

// Readiness must not be announced before the start-up listing of existing orders has been applied:
// an application that cancels or reconciles on onReady() would otherwise act on a table it believes
// is complete and silently leave those orders working on the venue.
TEST_F(ExchangeClientTest, OrdersOpenOnTheVenueAreAdoptedBeforeOnReadyFires) {
    fake.openOrdersResponse =
        R"([{"coin":"ETH","side":"A","limitPx":"3000.0","sz":"1.0","oid":4242,"timestamp":1700000000000,)"
        R"("origSz":"1.0","reduceOnly":false,"orderType":"Limit","tif":"Gtc","isTrigger":false,"triggerPx":"0.0"}])";
    std::size_t liveAtReady = 0;
    bool readyFlagAtReady = false;
    listener.onReadyHook = [&] {
        liveAtReady = clientUnderTest->liveOrders().size();
        readyFlagAtReady = clientUnderTest->isReady();
    };
    auto client = makeClient();
    clientUnderTest = client.get();
    client->start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready > 0; }));
    EXPECT_EQ(liveAtReady, 1U) << "cancelAll() from onReady() must see orders the venue already has open";
    EXPECT_TRUE(readyFlagAtReady) << "isReady() must agree with the callback";
}

// Spot support end to end through the client: `spotMeta` gives the markets, `spotClearinghouseState`
// the balances. The venue reports balances per token ("PURR") while positions are tracked per market
// ("PURR/USDC"), and on a unified account the spendable USDC lives here rather than in
// `clearinghouseState.accountValue`.
TEST_F(ExchangeClientTest, LoadSpotAssetsSeedsMarketsAndTokenBalances) {
    fake.serveSpot = true;
    auto client = makeClient(hl::ActionTransport::WebSocket, 2000, /*loadSpot=*/true);
    client->start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready > 0; }));

    const hl::AssetInfo* purr = client->assets().find("PURR/USDC");
    ASSERT_NE(purr, nullptr) << "spot markets must be in the registry";
    EXPECT_EQ(purr->kind, hl::AssetInfo::Kind::Spot);
    EXPECT_EQ(purr->asset, 10000U) << "spot wire id is 10000 + index";
    EXPECT_EQ(purr->baseToken, "PURR");

    EXPECT_EQ(client->spotTokenBalance("USDC"), d("1234.5678")) << "collateral on a unified account";
    EXPECT_EQ(client->spotTokenBalance("PURR"), d("500"));
    EXPECT_TRUE(client->spotTokenBalance("NOSUCHTOKEN").isZero());
    EXPECT_EQ(client->position("PURR/USDC"), d("500")) << "token balance seeds the market's position";
    // Perp positions from clearinghouseState are unaffected by spot loading.
    EXPECT_EQ(client->position("ETH"), d("-0.5"));
}

TEST_F(ExchangeClientTest, CancelsCarryTheFastFlagExceptForTriggerOrders) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    ASSERT_FALSE(client->cancel(cloid));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Canceled; }));
    EXPECT_NE(fake.posts.back().find(R"("f":true)"), std::string::npos) << fake.posts.back();

    hl::OrderRequest stop = btcBuy("49000");
    stop.trigger = hl::TriggerSpec{d("49000"), true, hl::TriggerSpec::Kind::StopLoss};
    const auto stopCloid = client->placeOrder(stop).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(stopCloid)->state == OrderState::Open; }));
    EXPECT_NE(fake.posts.back().find(R"("trigger")"), std::string::npos);
    ASSERT_FALSE(client->cancel(stopCloid));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(stopCloid)->state == OrderState::Canceled; }));
    EXPECT_EQ(fake.posts.back().find(R"("f":true)"), std::string::npos)
        << "the venue rejects fast cancels of trigger orders";
}

TEST_F(ExchangeClientTest, ModifyKeepsTheTriggerSpecification) {
    auto client = startReady();
    hl::OrderRequest stop = btcBuy("49000");
    stop.trigger = hl::TriggerSpec{d("49000"), true, hl::TriggerSpec::Kind::StopLoss};
    const auto cloid = client->placeOrder(stop).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    // Amending price and size keeps the trigger untouched — newPx is the limit price.
    ASSERT_FALSE(client->modify(cloid, d("48000"), d("0.003")));
    ASSERT_TRUE(runUntil(loop, [&] { return !client->findOrder(cloid)->modifyPending; }));
    EXPECT_NE(fake.posts.back().find(R"("trigger":{"isMarket":true,"triggerPx":"49000","tpsl":"sl"})"),
              std::string::npos)
        << "amending a stop must not turn it into a plain limit order: " << fake.posts.back();
    EXPECT_TRUE(client->findOrder(cloid)->isTrigger);
    EXPECT_EQ(client->findOrder(cloid)->px, d("48000"));

    // …and the trigger itself can be moved explicitly.
    ASSERT_FALSE(client->modify(cloid, d("47000"), d("0.003"),
                                hl::TriggerSpec{d("47500"), false, hl::TriggerSpec::Kind::StopLoss}));
    ASSERT_TRUE(runUntil(loop, [&] { return !client->findOrder(cloid)->modifyPending; }));
    EXPECT_NE(fake.posts.back().find(R"("trigger":{"isMarket":false,"triggerPx":"47500","tpsl":"sl"})"),
              std::string::npos)
        << fake.posts.back();
    ASSERT_TRUE(client->findOrder(cloid)->trigger.has_value());
    EXPECT_EQ(client->findOrder(cloid)->trigger->triggerPx, d("47500"));
}

TEST_F(ExchangeClientTest, SingleVenueErrorAppliesToTheWholeBatch) {
    // HL returns one error for the entire payload when pre-validation fails.
    fake.actionPayload = [](const std::string& type, const std::string&) -> std::string {
        if (type == "order") {
            return R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"error":"Insufficient margin to place order"}]}}})";
        }
        return {};
    };
    auto client = startReady();
    std::vector<hl::OrderRequest> reqs{btcBuy("50000"), btcBuy("49000"), btcBuy("48000")};
    auto placed = client->placeOrders(reqs);
    ASSERT_TRUE(placed);
    ASSERT_TRUE(runUntil(loop, [&] {
        for (const auto& c : *placed) {
            if (client->findOrder(c)->state != OrderState::Rejected) {
                return false;
            }
        }
        return true;
    }));
    for (const auto& c : *placed) {
        EXPECT_NE(client->findOrder(c)->lastError.find("Insufficient margin"), std::string::npos);
    }
    EXPECT_TRUE(client->liveOrders().empty()) << "no order may stay live after a batch-wide rejection";
}

TEST_F(ExchangeClientTest, PlacingFromInsideACallbackIsSafe) {
    auto client = startReady();
    // A strategy that reacts to a fill by quoting again — the classic re-entrant pattern.
    struct Reactor : hl::ExchangeListener {
        hl::ExchangeClient* ex{nullptr};
        hl::EventLoop* loop{nullptr};
        int placed{0};
        std::vector<hl::Cloid> cloids;
        void onFill(const hl::Fill&) override {
            if (placed < 3 && ex != nullptr) {
                hl::OrderRequest r;
                r.coin = "BTC";
                r.side = hl::Side::Buy;
                r.px = hl::Decimal::fromInt(40000);
                r.sz = hl::Decimal::parseOrZero("0.001");
                r.tif = hl::Tif::Alo;
                if (auto c = ex->placeOrder(r)) {
                    cloids.push_back(c.value());
                    ++placed;
                }
                loop->runOnce(5);  // the "wait for my order" idiom: re-enters the parser
            }
        }
    } reactor;
    reactor.loop = &loop;

    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    hl::ExchangeClient reactive(loop, reactor, cfg);
    reactor.ex = &reactive;
    reactive.start();
    ASSERT_TRUE(runUntil(loop, [&] { return reactive.isReady(); }));
    fake.pushFill(FakeHyperliquid::fillJson("BTC", "B", "0.001", "50000", "0", 500, 1, ""));
    ASSERT_TRUE(runUntil(loop, [&] { return reactor.placed == 1; }));
    for (const auto& c : reactor.cloids) {
        ASSERT_TRUE(runUntil(loop, [&] { return reactive.findOrder(c)->state == OrderState::Open; }));
    }
    EXPECT_EQ(reactive.position("BTC"), d("0.001"));
}

TEST_F(ExchangeClientTest, RestartAfterStopWorks) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    client->stop();
    EXPECT_FALSE(client->isReady());
    const int readyBefore = listener.ready;
    client->start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready > readyBefore; }));
    EXPECT_TRUE(client->isReady());
    const auto again = client->placeOrder(btcBuy("48000"));
    ASSERT_TRUE(again) << again.error().message;
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(again.value())->state == OrderState::Open; }));
}

TEST_F(ExchangeClientTest, LiquidationAndVenueCancelsAreReported) {
    struct RiskListener : Listener {
        std::optional<hl::LiquidationMsg> liquidation;
        void onLiquidation(const hl::LiquidationMsg& m) override { liquidation = m; }
    } risk;
    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    hl::ExchangeClient client(loop, risk, cfg);
    client.start();
    ASSERT_TRUE(runUntil(loop, [&] { return risk.ready > 0; }));
    const auto cloid = client.placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client.findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t oid = client.findOrder(cloid)->oid;

    fake.venue.wsBroadcast(R"({"channel":"user","data":{"nonUserCancel":[{"coin":"BTC","oid":)" +
                           std::to_string(oid) + R"(}]}})");
    ASSERT_TRUE(runUntil(loop, [&] { return client.findOrder(cloid)->state == OrderState::Canceled; }));
    EXPECT_EQ(client.findOrder(cloid)->lastError, "canceledByVenue");

    fake.venue.wsBroadcast(
        R"({"channel":"user","data":{"liquidation":{"lid":9,"liquidator":"0xa","liquidated_user":"0xb",)"
        R"("liquidated_ntl_pos":"500.0","liquidated_account_value":"0.0"}}})");
    ASSERT_TRUE(runUntil(loop, [&] { return risk.liquidation.has_value(); }));
    EXPECT_EQ(risk.liquidation->lid, 9U);
}

TEST_F(ExchangeClientTest, TracksTheAddressRequestBudget) {
    fake.rateLimitResponse = R"({"cumVlm":"1000.0","nRequestsUsed":50,"nRequestsCap":11000})";
    auto client = startReady();
    ASSERT_TRUE(runUntil(loop, [&] { return client->rateLimitStatus().requestsCap != 0; }));
    EXPECT_EQ(client->rateLimitStatus().requestsUsed, 50U);
    std::vector<hl::OrderRequest> batch{btcBuy("50000"), btcBuy("49000")};
    ASSERT_TRUE(client->placeOrders(batch));
    // Two orders in one action cost two address units, not one.
    EXPECT_EQ(client->rateLimitStatus().requestsUsed, 52U);
    EXPECT_EQ(client->stats().addressUnitsUsed, 2U);
}

TEST_F(ExchangeClientTest, TerminalOrdersAreEvictedWithTheirOidIndex) {
    hl::ExchangeConfig cfg;
    cfg.privateKey = kKey;
    cfg.restUrlOverride = fake.venue.httpUrl();
    cfg.wsUrlOverride = fake.venue.wsUrl();
    cfg.terminalOrderRetentionMs = 1;  // evict almost immediately
    hl::ExchangeClient client(loop, listener, cfg);
    client.start();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready > 0; }));
    const auto cloid = client.placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client.findOrder(cloid)->state == OrderState::Open; }));
    const std::uint64_t oid = client.findOrder(cloid)->oid;
    ASSERT_FALSE(client.cancel(cloid));
    ASSERT_TRUE(runUntil(loop, [&] { return client.findOrder(cloid)->state == OrderState::Canceled; }));
    // The eviction sweep runs every 10 s; wait for it.
    ASSERT_TRUE(runUntil(loop, [&] { return client.findOrder(cloid) == nullptr; }, 15'000));
    // A later update for the evicted oid must not resurrect anything.
    fake.pushOrderUpdate("BTC", "B", "50000", "0.002", "0.002", oid, "", "open");
    loop.runOnce(50);
    loop.runOnce(50);
    EXPECT_EQ(client.findOrder(cloid), nullptr);
    // It may appear as an external order, but never as the old one.
    for (const auto* o : client.liveOrders()) {
        EXPECT_TRUE(o->external);
    }
}

TEST_F(ExchangeClientTest, CancelByOidForAnUnknownOrderTouchesNothing) {
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }));
    const std::size_t updatesBefore = listener.updates.size();
    ASSERT_FALSE(client->cancelByOid("BTC", 999999));  // an oid this client never saw
    ASSERT_TRUE(runUntil(loop, [&] { return fake.posts.size() == 2; }));
    EXPECT_NE(fake.posts.back().find(R"("o":999999)"), std::string::npos);
    loop.runOnce(50);
    EXPECT_EQ(client->findOrder(cloid)->state, OrderState::Open) << "our own order must be untouched";
    EXPECT_EQ(listener.updates.size(), updatesBefore);
    EXPECT_TRUE(client->cancelByOid("NOSUCHCOIN", 1));
}

TEST_F(ExchangeClientTest, ActionInFlightWhenTheSocketDropsIsReconciled) {
    fake.autoRespond = false;  // the action never gets an answer over this socket
    auto client = startReady();
    const auto cloid = client->placeOrder(btcBuy()).value();
    ASSERT_TRUE(runUntil(loop, [&] { return fake.posts.size() == 1; }));
    fake.orderStatusByKey[cloid.toString()] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.002","oid":555,)"
        R"("timestamp":1,"origSz":"0.002","cloid":")" + cloid.toString() + R"("},"status":"open","statusTimestamp":2}})";
    fake.venue.dropWebSockets();
    // The pending post fails with Transport and the order is reconciled through /info.
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Open; }, 15'000));
    EXPECT_EQ(client->findOrder(cloid)->oid, 555U);
    EXPECT_GE(client->stats().reconciles, 1U);
    EXPECT_GE(client->stats().actionErrors, 1U);
}

TEST_F(ExchangeClientTest, InfoOverWebSocketDeliversTheResponse) {
    auto client = startReady();
    std::optional<hl::Result<std::string>> answer;
    client->infoOverWebSocket(R"({"type":"orderStatus","user":"0x0","oid":1})",
                              [&](const hl::Result<std::string>& r) { answer = r; });
    ASSERT_TRUE(runUntil(loop, [&] { return answer.has_value(); }));
    ASSERT_TRUE(answer->ok()) << answer->error().message;
    EXPECT_EQ(answer->value(), fake.wsInfoResponse);
}
