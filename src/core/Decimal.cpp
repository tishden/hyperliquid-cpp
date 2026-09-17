#include "hl/core/Decimal.h"
#include "hl/core/Int128.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <ostream>

namespace hl {

namespace {

constexpr std::int64_t kMaxRaw = std::numeric_limits<std::int64_t>::max();

}  // namespace

Decimal Decimal::fromDouble(double value) noexcept {
    return Decimal{static_cast<std::int64_t>(std::llround(value * static_cast<double>(kScale)))};
}

bool Decimal::parse(std::string_view text, Decimal& out) noexcept {
    const char* p = text.data();
    const char* end = p + text.size();
    if (p == end) {
        return false;
    }
    bool negative = false;
    if (*p == '-' || *p == '+') {
        negative = (*p == '-');
        ++p;
    }
    std::uint64_t mantissa = 0;
    bool anyDigit = false;
    // Integer part.
    while (p != end && static_cast<unsigned>(*p - '0') <= 9) {
        const auto digit = static_cast<std::uint64_t>(*p - '0');
        if (mantissa > (static_cast<std::uint64_t>(kMaxRaw) - digit) / 10) {
            return false;
        }
        mantissa = mantissa * 10 + digit;
        anyDigit = true;
        ++p;
    }
    int fracDigits = 0;
    if (p != end && *p == '.') {
        ++p;
        while (p != end && static_cast<unsigned>(*p - '0') <= 9) {
            if (fracDigits < kDecimals) {
                const auto digit = static_cast<std::uint64_t>(*p - '0');
                if (mantissa > (static_cast<std::uint64_t>(kMaxRaw) - digit) / 10) {
                    return false;
                }
                mantissa = mantissa * 10 + digit;
                ++fracDigits;
            }
            anyDigit = true;
            ++p;
        }
    }
    if (p != end || !anyDigit) {
        return false;
    }
    for (; fracDigits < kDecimals; ++fracDigits) {
        if (mantissa > static_cast<std::uint64_t>(kMaxRaw) / 10) {
            return false;
        }
        mantissa *= 10;
    }
    const auto signedRaw = static_cast<std::int64_t>(mantissa);
    out = Decimal{negative ? -signedRaw : signedRaw};
    return true;
}

std::size_t Decimal::toChars(char* out) const noexcept {
    std::uint64_t magnitude = raw_ < 0 ? static_cast<std::uint64_t>(-(raw_ + 1)) + 1U : static_cast<std::uint64_t>(raw_);
    std::uint64_t intPart = magnitude / static_cast<std::uint64_t>(kScale);
    std::uint64_t fracPart = magnitude % static_cast<std::uint64_t>(kScale);

    char buf[kMaxChars];
    std::size_t pos = sizeof(buf);
    // Fractional digits, trailing zeros trimmed.
    if (fracPart != 0) {
        int width = kDecimals;
        while (fracPart % 10 == 0) {
            fracPart /= 10;
            --width;
        }
        for (int i = 0; i < width; ++i) {
            buf[--pos] = static_cast<char>('0' + fracPart % 10);
            fracPart /= 10;
        }
        buf[--pos] = '.';
    }
    do {
        buf[--pos] = static_cast<char>('0' + intPart % 10);
        intPart /= 10;
    } while (intPart != 0);
    if (raw_ < 0) {
        buf[--pos] = '-';
    }
    const std::size_t len = sizeof(buf) - pos;
    std::memcpy(out, buf + pos, len);
    return len;
}

void Decimal::appendTo(std::string& out) const {
    char buf[kMaxChars];
    out.append(buf, toChars(buf));
}

std::string Decimal::toString() const {
    std::string s;
    appendTo(s);
    return s;
}

Decimal Decimal::mul(Decimal o) const noexcept {
    const Int128 product = static_cast<Int128>(raw_) * o.raw_;
    return Decimal{static_cast<std::int64_t>(product / kScale)};
}

Decimal Decimal::div(Decimal o) const noexcept {
    if (o.raw_ == 0) {
        return Decimal{};
    }
    const Int128 scaled = static_cast<Int128>(raw_) * kScale;
    return Decimal{static_cast<std::int64_t>(scaled / o.raw_)};
}

std::ostream& operator<<(std::ostream& os, Decimal value) { return os << value.toString(); }

Decimal roundToQuantum(Decimal value, std::int64_t quantumRaw, RoundingMode mode) noexcept {
    if (quantumRaw <= 1) {
        return value;
    }
    const std::int64_t raw = value.raw();
    std::int64_t floorMultiple = raw / quantumRaw;
    if (raw % quantumRaw != 0 && raw < 0) {
        --floorMultiple;
    }
    const std::int64_t down = floorMultiple * quantumRaw;
    if (down == raw) {
        return value;
    }
    const std::int64_t up = down + quantumRaw;
    switch (mode) {
        case RoundingMode::Down:
            return Decimal::fromRaw(down);
        case RoundingMode::Up:
            return Decimal::fromRaw(up);
        case RoundingMode::Nearest: {
            const std::int64_t distDown = raw - down;
            const std::int64_t distUp = up - raw;
            if (distDown == distUp) {
                return Decimal::fromRaw(raw >= 0 ? up : down);
            }
            return Decimal::fromRaw(distDown < distUp ? down : up);
        }
    }
    return value;
}

}  // namespace hl
