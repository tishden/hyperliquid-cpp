#include <gtest/gtest.h>

#include <string>

#include "hl/core/Hex.h"
#include "hl/crypto/Keccak.h"

namespace {
std::string hex(const hl::Hash256& h) { return hl::toHex(h.data(), h.size()); }
}  // namespace

TEST(Keccak256, KnownVectors) {
    EXPECT_EQ(hex(hl::keccak256("")), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    EXPECT_EQ(hex(hl::keccak256("abc")), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
    EXPECT_EQ(hex(hl::keccak256("The quick brown fox jumps over the lazy dog")),
              "4d741b6f1eb29cb2a9b9911c82f56fa8d73b04959d3d9d222895df6c0b28aa15");
}

TEST(Keccak256, ExactlyOneRateBlockAndBeyond) {
    // 135, 136 and 137 bytes straddle the 136-byte rate boundary (padding edge cases).
    EXPECT_EQ(hex(hl::keccak256(std::string(135, 'a'))),
              "34367dc248bbd832f4e3e69dfaac2f92638bd0bbd18f2912ba4ef454919cf446");
    EXPECT_EQ(hex(hl::keccak256(std::string(136, 'a'))),
              "a6c4d403279fe3e0af03729caada8374b5ca54d8065329a3ebcaeb4b60aa386e");
    EXPECT_EQ(hex(hl::keccak256(std::string(137, 'a'))),
              "d869f639c7046b4929fc92a4d988a8b22c55fbadb802c0c66ebcd484f1915f39");
}

TEST(Keccak256, IncrementalMatchesOneShot) {
    std::string data;
    for (int i = 0; i < 1000; ++i) {
        data.push_back(static_cast<char>(i * 31));
    }
    const auto expected = hl::keccak256(data);
    for (std::size_t split : {std::size_t{1}, std::size_t{7}, std::size_t{135}, std::size_t{136}, std::size_t{500}}) {
        hl::Keccak256 k;
        std::size_t pos = 0;
        while (pos < data.size()) {
            const std::size_t n = std::min(split, data.size() - pos);
            k.update(std::string_view{data}.substr(pos, n));
            pos += n;
        }
        EXPECT_EQ(k.finalize(), expected) << "split " << split;
    }
}
