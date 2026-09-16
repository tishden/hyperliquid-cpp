#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hl {

/// Lower-case hex of a byte range, without `0x`.
[[nodiscard]] std::string toHex(const std::uint8_t* data, std::size_t len);

/// Value of one hex digit, or -1.
[[nodiscard]] constexpr int hexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/// Strip an optional `0x`/`0X` prefix.
[[nodiscard]] constexpr std::string_view stripHexPrefix(std::string_view s) noexcept {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
    }
    return s;
}

/**
 * @brief Decode exactly @p outLen bytes of hex (optional `0x` prefix) into @p out.
 * @return false if the length does not match or a non-hex digit is present.
 */
[[nodiscard]] bool fromHex(std::string_view hex, std::uint8_t* out, std::size_t outLen) noexcept;

}  // namespace hl
