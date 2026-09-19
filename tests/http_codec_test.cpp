// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include <gtest/gtest.h>

#include "hl/net/HttpCodec.h"

using Status = hl::HttpResponseParser::Status;

namespace {
Status feedAll(hl::HttpResponseParser& p, std::string_view data, std::size_t& consumed) {
    return p.feed(data.data(), data.size(), consumed);
}
}  // namespace

TEST(HttpCodec, ContentLength) {
    hl::HttpResponseParser p;
    const std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"ok\":true}NEXT";
    std::size_t consumed = 0;
    ASSERT_EQ(feedAll(p, wire, consumed), Status::Complete);
    EXPECT_EQ(consumed, wire.size() - 4);
    EXPECT_EQ(p.response().status, 200);
    EXPECT_EQ(p.response().body, "{\"ok\":true}");
    EXPECT_TRUE(p.response().keepAlive);
}

TEST(HttpCodec, SplitAcrossReads) {
    const std::string wire = "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello";
    for (std::size_t cut = 1; cut < wire.size(); ++cut) {
        hl::HttpResponseParser p;
        std::size_t c1 = 0;
        std::size_t c2 = 0;
        ASSERT_EQ(p.feed(wire.data(), cut, c1), Status::NeedMore) << cut;
        EXPECT_EQ(c1, cut);
        ASSERT_EQ(p.feed(wire.data() + cut, wire.size() - cut, c2), Status::Complete) << cut;
        EXPECT_EQ(p.response().body, "hello");
    }
}

TEST(HttpCodec, Chunked) {
    const std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: 1\r\n\r\n";
    for (std::size_t step : {std::size_t{1}, std::size_t{3}, wire.size()}) {
        hl::HttpResponseParser p;
        Status st = Status::NeedMore;
        std::size_t pos = 0;
        while (pos < wire.size() && st == Status::NeedMore) {
            std::size_t consumed = 0;
            const std::size_t n = std::min(step, wire.size() - pos);
            st = p.feed(wire.data() + pos, n, consumed);
            pos += consumed;
        }
        ASSERT_EQ(st, Status::Complete) << step;
        EXPECT_EQ(p.response().body, "Wikipedia");
        EXPECT_EQ(pos, wire.size());
    }
}

TEST(HttpCodec, ConnectionCloseAndReadUntilClose) {
    hl::HttpResponseParser p;
    std::size_t consumed = 0;
    const std::string wire = "HTTP/1.1 422 Unprocessable Entity\r\nConnection: close\r\n\r\nFailed to deserialize";
    ASSERT_EQ(feedAll(p, wire, consumed), Status::NeedMore);
    ASSERT_EQ(p.finishOnClose(), Status::Complete);
    EXPECT_EQ(p.response().status, 422);
    EXPECT_FALSE(p.response().keepAlive);
    EXPECT_EQ(p.response().body, "Failed to deserialize");
}

TEST(HttpCodec, NoBodyStatusesAndErrors) {
    hl::HttpResponseParser p;
    std::size_t consumed = 0;
    ASSERT_EQ(feedAll(p, "HTTP/1.1 204 No Content\r\n\r\n", consumed), Status::Complete);
    EXPECT_TRUE(p.response().body.empty());

    hl::HttpResponseParser bad;
    EXPECT_EQ(feedAll(bad, "SSH-2.0-OpenSSH\r\n\r\n", consumed), Status::Error);
    hl::HttpResponseParser badLen;
    EXPECT_EQ(feedAll(badLen, "HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n", consumed), Status::Error);
    hl::HttpResponseParser truncated;
    EXPECT_EQ(feedAll(truncated, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", consumed), Status::NeedMore);
    EXPECT_EQ(truncated.finishOnClose(), Status::Error);
}

TEST(HttpCodec, RetryAfterHeader) {
    hl::HttpResponseParser p;
    std::size_t consumed = 0;
    const std::string wire = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7\r\nContent-Length: 3\r\n\r\nno!";
    ASSERT_EQ(p.feed(wire.data(), wire.size(), consumed), Status::Complete);
    EXPECT_EQ(p.response().status, 429);
    EXPECT_EQ(p.response().retryAfterSeconds, 7);
    hl::HttpResponseParser plain;
    ASSERT_EQ(plain.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 38, consumed), Status::Complete);
    EXPECT_EQ(plain.response().retryAfterSeconds, 0);
}
