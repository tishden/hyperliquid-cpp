// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hl/net/WsCodec.h"

namespace {

struct Sink final : hl::WsFrameSink {
    std::vector<std::pair<hl::WsOpcode, std::string>> messages;
    std::vector<std::pair<hl::WsOpcode, std::string>> controls;
    void onWsMessage(hl::WsOpcode op, std::string_view p) override { messages.emplace_back(op, std::string{p}); }
    void onWsControl(hl::WsOpcode op, std::string_view p) override { controls.emplace_back(op, std::string{p}); }
};

// Server frame (unmasked).
std::string serverFrame(std::uint8_t firstByte, std::string_view payload) {
    std::string f;
    f.push_back(static_cast<char>(firstByte));
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        f.push_back(126);
        f.push_back(static_cast<char>(n >> 8));
        f.push_back(static_cast<char>(n));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<char>(static_cast<std::uint64_t>(n) >> (8 * i)));
        }
    }
    f.append(payload);
    return f;
}

}  // namespace

TEST(WsCodec, AcceptKeyRfc6455Example) {
    EXPECT_EQ(hl::wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsCodec, Base64) {
    auto b = [](std::string_view s) { return hl::base64Encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); };
    EXPECT_EQ(b(""), "");
    EXPECT_EQ(b("f"), "Zg==");
    EXPECT_EQ(b("fo"), "Zm8=");
    EXPECT_EQ(b("foo"), "Zm9v");
    EXPECT_EQ(b("foobar"), "Zm9vYmFy");
}

TEST(WsCodec, EncodeIsMaskedAndDecodable) {
    for (std::size_t len : {std::size_t{0}, std::size_t{5}, std::size_t{125}, std::size_t{126}, std::size_t{65535},
                            std::size_t{65536}, std::size_t{200000}}) {
        std::string payload(len, '\0');
        for (std::size_t i = 0; i < len; ++i) {
            payload[i] = static_cast<char>('a' + i % 26);
        }
        std::string wire;
        hl::encodeWsFrame(hl::WsOpcode::Text, payload, 0x12345678, wire);
        EXPECT_EQ(static_cast<std::uint8_t>(wire[0]), 0x81);
        EXPECT_NE(static_cast<std::uint8_t>(wire[1]) & 0x80, 0) << "client frames must be masked";
        Sink sink;
        hl::WsFrameDecoder dec;
        ASSERT_EQ(dec.feed(wire.data(), wire.size(), sink), hl::WsFrameDecoder::Status::Ok);
        ASSERT_EQ(sink.messages.size(), 1U) << len;
        EXPECT_EQ(sink.messages[0].second, payload);
    }
}

TEST(WsCodec, ByteByByteFeedAndMultipleFramesPerRead) {
    std::string wire = serverFrame(0x81, "hello") + serverFrame(0x89, "p") + serverFrame(0x81, std::string(300, 'x'));
    Sink a;
    hl::WsFrameDecoder dec;
    for (char c : wire) {
        ASSERT_EQ(dec.feed(&c, 1, a), hl::WsFrameDecoder::Status::Ok);
    }
    ASSERT_EQ(a.messages.size(), 2U);
    EXPECT_EQ(a.messages[0].second, "hello");
    EXPECT_EQ(a.messages[1].second.size(), 300U);
    ASSERT_EQ(a.controls.size(), 1U);
    EXPECT_EQ(a.controls[0].first, hl::WsOpcode::Ping);

    Sink b;
    hl::WsFrameDecoder dec2;
    ASSERT_EQ(dec2.feed(wire.data(), wire.size(), b), hl::WsFrameDecoder::Status::Ok);
    EXPECT_EQ(b.messages.size(), 2U);
}

TEST(WsCodec, FragmentedMessageWithInterleavedControl) {
    const std::string wire = serverFrame(0x01, "frag-") + serverFrame(0x89, "") + serverFrame(0x00, "ment") +
                             serverFrame(0x80, "ed");
    Sink s;
    hl::WsFrameDecoder dec;
    ASSERT_EQ(dec.feed(wire.data(), wire.size(), s), hl::WsFrameDecoder::Status::Ok);
    ASSERT_EQ(s.messages.size(), 1U);
    EXPECT_EQ(s.messages[0].first, hl::WsOpcode::Text);
    EXPECT_EQ(s.messages[0].second, "frag-mented");
    EXPECT_EQ(s.controls.size(), 1U);
}

TEST(WsCodec, ProtocolErrors) {
    Sink s;
    hl::WsFrameDecoder dec;
    const std::string orphanContinuation = serverFrame(0x80, "x");
    EXPECT_EQ(dec.feed(orphanContinuation.data(), orphanContinuation.size(), s), hl::WsFrameDecoder::Status::ProtocolError);

    hl::WsFrameDecoder dec2;
    const std::string fragmentedPing = serverFrame(0x09, "x");
    EXPECT_EQ(dec2.feed(fragmentedPing.data(), fragmentedPing.size(), s), hl::WsFrameDecoder::Status::ProtocolError);

    hl::WsFrameDecoder small(10);
    const std::string big = serverFrame(0x81, std::string(11, 'y'));
    EXPECT_EQ(small.feed(big.data(), big.size(), s), hl::WsFrameDecoder::Status::MessageTooBig);
}

TEST(WsCodec, ResetInsideCallbackIsSafe) {
    struct ResettingSink final : hl::WsFrameSink {
        hl::WsFrameDecoder* dec{nullptr};
        int calls{0};
        void onWsMessage(hl::WsOpcode, std::string_view) override {
            ++calls;
            dec->reset();
        }
        void onWsControl(hl::WsOpcode, std::string_view) override {}
    } sink;
    hl::WsFrameDecoder dec;
    sink.dec = &dec;
    const std::string wire = serverFrame(0x81, "a") + serverFrame(0x81, "b");
    EXPECT_EQ(dec.feed(wire.data(), wire.size(), sink), hl::WsFrameDecoder::Status::Ok);
    EXPECT_EQ(sink.calls, 1);
}
