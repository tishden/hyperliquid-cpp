// End-to-end tests of ExchangeClient against an in-process MockVenue (real sockets,
// real HTTP/WebSocket stack, scripted venue responses).
#include <gtest/gtest.h>

#include <map>
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
        if (body.find("clearinghouseState") != std::string::npos) {
            return R"({"marginSummary":{"accountValue":"1000.5","totalNtlPos":"1500","totalRawUsd":"2500","totalMarginUsed":"75"},)"
                   R"("crossMarginSummary":{},"withdrawable":"900","assetPositions":[{"type":"oneWay","position":{"coin":"ETH",)"
                   R"("szi":"-0.5","entryPx":"3000","positionValue":"1500","unrealizedPnl":"-2","returnOnEquity":"0",)"
                   R"("liquidationPx":null,"marginUsed":"75","leverage":{"type":"cross","value":20}}}],"time":1})";
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
    int disconnects{0};
    std::vector<hl::Order> updates;
    std::vector<hl::Fill> fills;
    std::vector<hl::Error> errors;
    void onReady() override { ++ready; }
    void onDisconnected(std::string_view) override { ++disconnects; }
    void onOrderUpdate(const hl::Order& o) override { updates.push_back(o); }
    void onFill(const hl::Fill& f) override { fills.push_back(f); }
    void onError(const hl::Error& e) override { errors.push_back(e); }
};

class ExchangeClientTest : public ::testing::Test {
protected:
    void SetUp() override { hl::setLogLevel(hl::LogLevel::Error); }

    std::unique_ptr<hl::ExchangeClient> makeClient(hl::ActionTransport transport = hl::ActionTransport::WebSocket,
                                                   std::int64_t timeoutMs = 2000) {
        hl::ExchangeConfig cfg;
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
    fake.orderStatusByKey[cloid.toString()] =
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

    fake.orderStatusByKey[cloid.toString()] =
        R"({"status":"order","order":{"order":{"coin":"BTC","side":"B","limitPx":"50000.0","sz":"0.002","oid":1000,"timestamp":1,)"
        R"("origSz":"0.002","cloid":")" + cloid.toString() + R"("},"status":"marginCanceled","statusTimestamp":2}})";
    fake.venue.dropWebSockets();
    ASSERT_TRUE(runUntil(loop, [&] { return listener.disconnects == 1; }));
    EXPECT_FALSE(client->isReady());
    ASSERT_TRUE(runUntil(loop, [&] { return listener.ready == 2; }));
    ASSERT_TRUE(runUntil(loop, [&] { return client->findOrder(cloid)->state == OrderState::Canceled; }));
    EXPECT_EQ(client->findOrder(cloid)->lastError, "marginCanceled");
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

TEST_F(ExchangeClientTest, RejectsMalformedConfig) {
    hl::ExchangeConfig cfg;
    EXPECT_THROW(hl::ExchangeClient(loop, listener, cfg), std::invalid_argument);
    cfg.privateKey = kKey;
    cfg.vaultAddress = "0x12";
    EXPECT_THROW(hl::ExchangeClient(loop, listener, cfg), std::invalid_argument);
}
