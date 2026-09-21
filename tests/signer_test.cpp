// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
// Golden vectors: hyperliquid-python-sdk (tests/signing_test.py and vectors generated
// with hyperliquid.utils.signing.sign_l1_action), private key kKey.
#include <gtest/gtest.h>

#include <stdexcept>

#include "hl/core/Hex.h"
#include "hl/crypto/Signer.h"
#include "hl/om/MsgPack.h"

namespace {

constexpr std::string_view kKey = "0x0123456789012345678901234567890123456789012345678901234567890123";

std::string pad64(std::string_view h) {
    h = hl::stripHexPrefix(h);
    return std::string(64 - h.size(), '0') + std::string{h};
}

void expectSig(const hl::Signature& sig, std::string_view r, std::string_view s, int v) {
    EXPECT_EQ(hl::toHex(sig.r.data(), 32), pad64(r));
    EXPECT_EQ(hl::toHex(sig.s.data(), 32), pad64(s));
    EXPECT_EQ(sig.v, v);
}

std::vector<std::uint8_t> dummyAction(std::uint64_t num) {
    hl::MsgPackWriter w;
    w.mapHeader(2);
    w.str("type");
    w.str("dummy");
    w.str("num");
    w.uint(num);
    return w.release();
}

std::vector<std::uint8_t> limitOrderAction(std::uint64_t asset, bool isBuy, std::string_view px, std::string_view sz,
                                           std::string_view tif, std::string_view cloid = {}) {
    hl::MsgPackWriter w;
    w.mapHeader(3);
    w.str("type");
    w.str("order");
    w.str("orders");
    w.arrayHeader(1);
    w.mapHeader(cloid.empty() ? 6 : 7);
    w.str("a");
    w.uint(asset);
    w.str("b");
    w.boolean(isBuy);
    w.str("p");
    w.str(px);
    w.str("s");
    w.str(sz);
    w.str("r");
    w.boolean(false);
    w.str("t");
    w.mapHeader(1);
    w.str("limit");
    w.mapHeader(1);
    w.str("tif");
    w.str(tif);
    if (!cloid.empty()) {
        w.str("c");
        w.str(cloid);
    }
    w.str("grouping");
    w.str("na");
    return w.release();
}

}  // namespace

TEST(Signer, DerivesAddress) {
    hl::Signer signer{kKey};
    EXPECT_EQ(hl::toHex(signer.address()), "0x14791697260e4c9a71f18484c9f997b308e59325");
    hl::Signer bare{kKey.substr(2)};
    EXPECT_EQ(bare.address(), signer.address());
}

TEST(Signer, RejectsBadKeys) {
    EXPECT_THROW(hl::Signer{"0x1234"}, std::invalid_argument);
    EXPECT_THROW(hl::Signer{std::string(64, 'z')}, std::invalid_argument);
    EXPECT_THROW(hl::Signer{std::string(64, '0')}, std::invalid_argument);  // zero scalar
    EXPECT_THROW(hl::Signer{"fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141"},
                 std::invalid_argument);  // == curve order
}

TEST(Signer, ConnectionIdMatchesProduction) {
    const auto action = limitOrderAction(4, true, "1670.1", "0.0147", "Ioc");
    const auto h = hl::actionHash(action, std::nullopt, 1677777606040ULL, std::nullopt);
    EXPECT_EQ(hl::toHex(h.data(), 32), "0fcbeda5ae3c4950a548021552a4fea2226858c4453571bf3f24ba017eac2908");
}

TEST(Signer, DummyActionMainnetAndTestnet) {
    hl::Signer signer{kKey};
    const auto action = dummyAction(100000000000ULL);
    expectSig(signer.signL1Action(action, std::nullopt, 0, std::nullopt, true),
              "0x53749d5b30552aeb2fca34b530185976545bb22d0b3ce6f62e31be961a59298",
              "0x755c40ba9bf05223521753995abb2f73ab3229be8ec921f350cb447e384d8ed8", 27);
    expectSig(signer.signL1Action(action, std::nullopt, 0, std::nullopt, false),
              "0x542af61ef1f429707e3c76c5293c80d01f74ef853e34b76efffcb57e574f9510",
              "0x17b8b32f086e8cdede991f1e2c529f5dd5297cbe8128500e00cbaf766204a613", 28);
}

TEST(Signer, OrderAction) {
    hl::Signer signer{kKey};
    const auto action = limitOrderAction(1, true, "100", "100", "Gtc");
    expectSig(signer.signL1Action(action, std::nullopt, 0, std::nullopt, true),
              "0xd65369825a9df5d80099e513cce430311d7d26ddf477f5b3a33d2806b100d78e",
              "0x2b54116ff64054968aa237c20ca9ff68000f977c93289157748a3162b6ea940e", 28);
    expectSig(signer.signL1Action(action, std::nullopt, 0, std::nullopt, false),
              "0x82b2ba28e76b3d761093aaded1b1cdad4960b3af30212b343fb2e6cdfa4e3d54",
              "0x6b53878fc99d26047f4d7e8c90eb98955a109f44209163f52d8dc4278cbbd9f5", 27);
}

TEST(Signer, OrderActionWithCloid) {
    hl::Signer signer{kKey};
    const auto action = limitOrderAction(1, true, "100", "100", "Gtc", "0x00000000000000000000000000000001");
    expectSig(signer.signL1Action(action, std::nullopt, 0, std::nullopt, true),
              "0x41ae18e8239a56cacbc5dad94d45d0b747e5da11ad564077fcac71277a946e3",
              "0x3c61f667e747404fe7eea8f90ab0e76cc12ce60270438b2058324681a00116da", 27);
    expectSig(signer.signL1Action(action, std::nullopt, 0, std::nullopt, false),
              "0xeba0664bed2676fc4e5a743bf89e5c7501aa6d870bdb9446e122c9466c5cd16d",
              "0x7f3e74825c9114bc59086f1eebea2928c190fdfbfde144827cb02b85bbe90988", 28);
}

TEST(Signer, VaultAddress) {
    hl::Signer signer{kKey};
    const auto action = dummyAction(100000000000ULL);
    const auto vault = hl::parseAddress("0x1719884eb866cb12b2287399b15f7db5e7d775ea");
    ASSERT_TRUE(vault);
    expectSig(signer.signL1Action(action, vault, 0, std::nullopt, true),
              "0x3c548db75e479f8012acf3000ca3a6b05606bc2ec0c29c50c515066a326239",
              "0x4d402be7396ce74fbba3795769cda45aec00dc3125a984f2a9f23177b190da2c", 28);
    expectSig(signer.signL1Action(action, vault, 0, std::nullopt, false),
              "0xe281d2fb5c6e25ca01601f878e4d69c965bb598b88fac58e475dd1f5e56c362b",
              "0x7ddad27e9a238d045c035bc606349d075d5c5cd00a6cd1da23ab5c39d4ef0f60", 27);
}

TEST(Signer, DeterministicSignatures) {
    hl::Signer signer{kKey};
    const auto action = dummyAction(42);
    const auto a = signer.signL1Action(action, std::nullopt, 7, std::nullopt, true);
    const auto b = signer.signL1Action(action, std::nullopt, 7, std::nullopt, true);
    EXPECT_EQ(a.r, b.r);
    EXPECT_EQ(a.s, b.s);
    // low-s: top bit of s is clear
    EXPECT_LT(a.s[0], 0x80);
}

TEST(MsgPack, IntegerEncodingMatchesMsgpackPython) {
    auto u = [](std::uint64_t v) {
        hl::MsgPackWriter w;
        w.uint(v);
        return hl::toHex(w.bytes().data(), w.bytes().size());
    };
    EXPECT_EQ(u(0), "00");
    EXPECT_EQ(u(127), "7f");
    EXPECT_EQ(u(128), "cc80");
    EXPECT_EQ(u(255), "ccff");
    EXPECT_EQ(u(256), "cd0100");
    EXPECT_EQ(u(65536), "ce00010000");
    EXPECT_EQ(u(4294967296ULL), "cf0000000100000000");
    auto s = [](std::int64_t v) {
        hl::MsgPackWriter w;
        w.sint(v);
        return hl::toHex(w.bytes().data(), w.bytes().size());
    };
    EXPECT_EQ(s(-1), "ff");
    EXPECT_EQ(s(-32), "e0");
    EXPECT_EQ(s(-33), "d0df");
    EXPECT_EQ(s(-129), "d1ff7f");
    EXPECT_EQ(s(-40000), "d2ffff63c0");
    EXPECT_EQ(s(5), "05");
}

TEST(MsgPack, StringAndContainerHeaders) {
    hl::MsgPackWriter w;
    w.str(std::string(31, 'x'));
    EXPECT_EQ(w.bytes()[0], 0xbf);
    w.clear();
    w.str(std::string(32, 'x'));
    EXPECT_EQ(w.bytes()[0], 0xd9);
    w.clear();
    w.mapHeader(16);
    EXPECT_EQ(hl::toHex(w.bytes().data(), w.bytes().size()), "de0010");
    w.clear();
    w.arrayHeader(15);
    EXPECT_EQ(hl::toHex(w.bytes().data(), w.bytes().size()), "9f");
}
