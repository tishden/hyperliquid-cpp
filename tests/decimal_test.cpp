// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include <gtest/gtest.h>

#include "hl/core/Decimal.h"

using hl::Decimal;
using hl::RoundingMode;

namespace {
Decimal d(std::string_view s) {
    Decimal out;
    EXPECT_TRUE(Decimal::parse(s, out)) << s;
    return out;
}
}  // namespace

TEST(Decimal, ParsesVenueStrings) {
    EXPECT_EQ(d("75951.0").raw(), 7'595'100'000'000);
    EXPECT_EQ(d("0.00056").raw(), 56'000);
    EXPECT_EQ(d("12").raw(), 1'200'000'000);
    EXPECT_EQ(d("-0.0001711314").raw(), -17'113);  // 10 decimals truncated to 8
    EXPECT_EQ(d("+1.5").raw(), 150'000'000);
    EXPECT_EQ(d(".5").raw(), 50'000'000);
    EXPECT_EQ(d("5.").raw(), 500'000'000);
    EXPECT_EQ(d("0").raw(), 0);
}

TEST(Decimal, RejectsMalformed) {
    Decimal out = Decimal::fromInt(7);
    for (std::string_view bad : {"", "-", ".", "1e5", "1.2.3", "abc", " 1", "1 ", "--1"}) {
        EXPECT_FALSE(Decimal::parse(bad, out)) << bad;
    }
    EXPECT_EQ(out, Decimal::fromInt(7));  // untouched on failure
}

TEST(Decimal, RejectsOverflow) {
    Decimal out;
    EXPECT_TRUE(Decimal::parse("92233720368.54775807", out));
    EXPECT_FALSE(Decimal::parse("92233720368.54775808", out));
    EXPECT_FALSE(Decimal::parse("100000000000", out));
}

TEST(Decimal, FormatsCanonicalWireForm) {
    EXPECT_EQ(d("50000.0").toString(), "50000");
    EXPECT_EQ(d("0.00100").toString(), "0.001");
    EXPECT_EQ(d("1670.10").toString(), "1670.1");
    EXPECT_EQ(d("0").toString(), "0");
    EXPECT_EQ(d("-0.5").toString(), "-0.5");
    EXPECT_EQ(d("0.00000001").toString(), "0.00000001");
    EXPECT_EQ(Decimal::fromRaw(INT64_MIN).toString(), "-92233720368.54775808");
}

TEST(Decimal, RoundTripsThroughText) {
    for (std::string_view s : {"1", "0.1", "123.456", "99999.99999999", "-3.14159265"}) {
        EXPECT_EQ(d(d(s).toString()), d(s));
    }
}

TEST(Decimal, Arithmetic) {
    EXPECT_EQ(d("1.5") + d("2.25"), d("3.75"));
    EXPECT_EQ(d("1.5") - d("2.25"), d("-0.75"));
    EXPECT_EQ(d("75951.5").mul(d("0.001")), d("75.9515"));
    EXPECT_EQ(d("10").div(d("4")), d("2.5"));
    EXPECT_EQ(d("1").div(d("3")), d("0.33333333"));
    EXPECT_EQ(d("1").div(Decimal{}), Decimal{});
    EXPECT_LT(d("-1"), d("0.00000001"));
}

TEST(Decimal, FromDouble) {
    EXPECT_EQ(Decimal::fromDouble(0.1), d("0.1"));
    EXPECT_EQ(Decimal::fromDouble(-2.000000005), d("-2.00000001"));
}

TEST(Decimal, RoundToQuantum) {
    const std::int64_t tick = d("0.5").raw();
    EXPECT_EQ(hl::roundToQuantum(d("10.7"), tick, RoundingMode::Down), d("10.5"));
    EXPECT_EQ(hl::roundToQuantum(d("10.7"), tick, RoundingMode::Up), d("11"));
    EXPECT_EQ(hl::roundToQuantum(d("10.7"), tick, RoundingMode::Nearest), d("10.5"));
    EXPECT_EQ(hl::roundToQuantum(d("10.75"), tick, RoundingMode::Nearest), d("11"));
    EXPECT_EQ(hl::roundToQuantum(d("-10.7"), tick, RoundingMode::Down), d("-11"));
    EXPECT_EQ(hl::roundToQuantum(d("-10.7"), tick, RoundingMode::Up), d("-10.5"));
    EXPECT_EQ(hl::roundToQuantum(d("-10.75"), tick, RoundingMode::Nearest), d("-11"));
    EXPECT_EQ(hl::roundToQuantum(d("10.5"), tick, RoundingMode::Up), d("10.5"));
}

TEST(Decimal, ArithmeticSaturatesInsteadOfWrapping) {
    const Decimal big = d("92233720368.5");  // close to the representation limit
    EXPECT_EQ(big.mul(Decimal::fromInt(1000)), Decimal::max());
    EXPECT_EQ(big.mul(Decimal::fromInt(-1000)), Decimal::min());
    EXPECT_TRUE(big.mul(Decimal::fromInt(1000)).isSaturated());
    EXPECT_FALSE(d("2").mul(d("3")).isSaturated());
    EXPECT_EQ(d("2").mul(d("3")), d("6"));
    // Dividing by a tiny number overflows the mantissa: saturate rather than wrap to a negative.
    EXPECT_EQ(Decimal::fromInt(1000).div(d("0.00000001")), Decimal::max());
    EXPECT_EQ(Decimal::fromInt(-1000).div(d("0.00000001")), Decimal::min());
    EXPECT_EQ(Decimal::max().toString(), "92233720368.54775807");
    EXPECT_EQ(Decimal::min().toString(), "-92233720368.54775808");
}
