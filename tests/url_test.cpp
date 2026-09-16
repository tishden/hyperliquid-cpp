#include <gtest/gtest.h>

#include "hl/core/Types.h"
#include "hl/net/Url.h"

TEST(Url, ParsesVariants) {
    auto u = hl::Url::parse("wss://api.hyperliquid-testnet.xyz/ws");
    ASSERT_TRUE(u);
    EXPECT_TRUE(u->secure);
    EXPECT_EQ(u->host, "api.hyperliquid-testnet.xyz");
    EXPECT_EQ(u->port, 443);
    EXPECT_EQ(u->path, "/ws");

    auto h = hl::Url::parse("http://127.0.0.1:8080");
    ASSERT_TRUE(h);
    EXPECT_FALSE(h->secure);
    EXPECT_EQ(h->port, 8080);
    EXPECT_EQ(h->path, "/");

    EXPECT_FALSE(hl::Url::parse("ftp://x"));
    EXPECT_FALSE(hl::Url::parse("https://"));
    EXPECT_FALSE(hl::Url::parse("http://host:99999"));
    EXPECT_FALSE(hl::Url::parse("no-scheme"));
}

TEST(Url, NetworkEndpoints) {
    EXPECT_EQ(hl::restUrl(hl::Network::Mainnet), "https://api.hyperliquid.xyz");
    EXPECT_EQ(hl::wsUrl(hl::Network::Testnet), "wss://api.hyperliquid-testnet.xyz/ws");
}

TEST(Address, ParseAndFormat) {
    auto a = hl::parseAddress("0x14791697260E4C9A71F18484C9F997B308E59325");
    ASSERT_TRUE(a);
    EXPECT_EQ(hl::toHex(*a), "0x14791697260e4c9a71f18484c9f997b308e59325");
    EXPECT_FALSE(hl::parseAddress("0x1234"));
}
