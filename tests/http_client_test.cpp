// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
// HttpClient behaviour against the mock server: ordering, keep-alive, timeouts, rate limiting.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hl/core/Log.h"
#include "hl/net/HttpClient.h"
#include "support/MockVenue.h"

using hltest::runUntil;

namespace {

class HttpClientTest : public ::testing::Test {
protected:
    void SetUp() override { hl::setLogLevel(hl::LogLevel::Error); }

    hl::EventLoop loop;
    hltest::MockVenue venue{loop};
};

}  // namespace

TEST_F(HttpClientTest, RequestsCompleteInSubmissionOrderOnOneConnection) {
    venue.onHttp = [](const std::string& /*path*/, const std::string& body) -> hltest::MockVenue::HttpReply {
        return {200, "echo:" + body};
    };
    hl::HttpClient client(loop, venue.httpUrl());
    std::vector<std::string> bodies;
    for (int i = 0; i < 5; ++i) {
        client.postJson("/info", std::to_string(i), [&](const hl::Error& e, const hl::HttpResponse& r) {
            ASSERT_FALSE(e) << e.message;
            bodies.push_back(r.body);
        });
    }
    ASSERT_TRUE(runUntil(loop, [&] { return bodies.size() == 5; }));
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(bodies[static_cast<std::size_t>(i)], "echo:" + std::to_string(i));
    }
    EXPECT_EQ(venue.openWebSockets(), 0);
    EXPECT_EQ(client.pending(), 0U);
}

TEST_F(HttpClientTest, RateLimitPausesTheQueueForRetryAfter) {
    int served = 0;
    venue.onHttp = [&](const std::string&, const std::string&) -> hltest::MockVenue::HttpReply {
        ++served;
        return served == 1 ? hltest::MockVenue::HttpReply{429, "slow down"} : hltest::MockVenue::HttpReply{200, "ok"};
    };
    hl::HttpClientOptions options;
    options.defaultRateLimitPauseMs = 200;  // no Retry-After header in the mock reply
    hl::HttpClient client(loop, venue.httpUrl(), options);
    std::vector<int> statuses;
    client.postJson("/info", "a", [&](const hl::Error& e, const hl::HttpResponse& r) {
        EXPECT_EQ(e.kind, hl::Error::Kind::Http);
        statuses.push_back(r.status);
    });
    client.postJson("/info", "b", [&](const hl::Error&, const hl::HttpResponse& r) { statuses.push_back(r.status); });
    ASSERT_TRUE(runUntil(loop, [&] { return statuses.size() == 1; }));
    EXPECT_EQ(client.rateLimitHits(), 1U);
    EXPECT_GT(client.pausedUntilMs(), 0);
    const std::int64_t start = hl::EventLoop::nowMs();
    ASSERT_TRUE(runUntil(loop, [&] { return statuses.size() == 2; }, 5000));
    EXPECT_GE(hl::EventLoop::nowMs() - start, 150) << "the second request must wait out the pause";
    EXPECT_EQ(statuses[1], 200);
}

TEST_F(HttpClientTest, UnreachableEndpointFailsAndAnotherClientStillWorks) {
    venue.onHttp = [](const std::string&, const std::string&) -> hltest::MockVenue::HttpReply {
        return {200, "late"};
    };
    hl::HttpClientOptions options;
    options.requestTimeoutMs = 150;
    hl::HttpClient client(loop, venue.httpUrl(), options);
    // Requests to a port nobody listens on time out at connect.
    hl::HttpClient dead(loop, "http://127.0.0.1:9", options);
    hl::Error failure;
    bool done = false;
    dead.postJson("/info", "x", [&](const hl::Error& e, const hl::HttpResponse&) {
        failure = e;
        done = true;
    });
    ASSERT_TRUE(runUntil(loop, [&] { return done; }, 8000));
    EXPECT_TRUE(failure);
    EXPECT_TRUE(failure.kind == hl::Error::Kind::Transport || failure.kind == hl::Error::Kind::Timeout);

    bool ok = false;
    client.postJson("/info", "y", [&](const hl::Error& e, const hl::HttpResponse& r) {
        ok = !e && r.body == "late";
    });
    ASSERT_TRUE(runUntil(loop, [&] { return ok; }));
}

TEST_F(HttpClientTest, CancelAllFailsQueuedRequests) {
    venue.onHttp = [](const std::string&, const std::string&) -> hltest::MockVenue::HttpReply {
        return {200, "ok"};
    };
    hl::HttpClient client(loop, venue.httpUrl());
    int failures = 0;
    for (int i = 0; i < 3; ++i) {
        client.postJson("/info", "x", [&](const hl::Error& e, const hl::HttpResponse&) {
            failures += e ? 1 : 0;
        });
    }
    client.cancelAll("shutting down");
    EXPECT_EQ(failures, 3);
    EXPECT_EQ(client.pending(), 0U);
}

TEST_F(HttpClientTest, OutboxLimitFailsInsteadOfGrowingForever) {
    venue.onHttp = [](const std::string&, const std::string&) -> hltest::MockVenue::HttpReply { return {200, "ok"}; };
    hl::HttpClientOptions options;
    options.tls.maxOutboxBytes = 4096;  // smaller than the request below
    hl::HttpClient client(loop, venue.httpUrl(), options);
    bool failed = false;
    client.postJson("/info", std::string(64 * 1024, 'x'), [&](const hl::Error& e, const hl::HttpResponse&) {
        failed = static_cast<bool>(e);
    });
    ASSERT_TRUE(runUntil(loop, [&] { return failed; }, 5000));
}
