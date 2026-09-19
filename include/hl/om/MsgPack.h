// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace hl {

/**
 * @brief Minimal MessagePack encoder that reproduces `msgpack-python` byte-for-byte.
 *
 * Hyperliquid signs `keccak(msgpack(action) ‖ …)`, so the encoding must match the
 * reference SDK exactly: map keys in insertion order and the *smallest* integer /
 * string / container header. Only the types used by HL actions are provided.
 */
class MsgPackWriter {
public:
    MsgPackWriter() { buf_.reserve(256); }

    void mapHeader(std::size_t n) { collectionHeader(n, 0x80, 0xde, 0xdf); }
    void arrayHeader(std::size_t n) { collectionHeader(n, 0x90, 0xdc, 0xdd); }

    void str(std::string_view s) {
        const std::size_t len = s.size();
        if (len < 32) {
            put(static_cast<std::uint8_t>(0xa0 | len));
        } else if (len <= 0xFF) {
            put(0xd9);
            put(static_cast<std::uint8_t>(len));
        } else if (len <= 0xFFFF) {
            put(0xda);
            be(static_cast<std::uint16_t>(len));
        } else {
            put(0xdb);
            be(static_cast<std::uint32_t>(len));
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
        buf_.insert(buf_.end(), p, p + len);
    }

    void boolean(bool b) { put(b ? 0xc3 : 0xc2); }
    void nil() { put(0xc0); }

    void uint(std::uint64_t v) {
        if (v < 0x80) {
            put(static_cast<std::uint8_t>(v));
        } else if (v <= 0xFF) {
            put(0xcc);
            put(static_cast<std::uint8_t>(v));
        } else if (v <= 0xFFFF) {
            put(0xcd);
            be(static_cast<std::uint16_t>(v));
        } else if (v <= 0xFFFFFFFFULL) {
            put(0xce);
            be(static_cast<std::uint32_t>(v));
        } else {
            put(0xcf);
            be(v);
        }
    }

    void sint(std::int64_t v) {
        if (v >= 0) {
            uint(static_cast<std::uint64_t>(v));
        } else if (v >= -32) {
            put(static_cast<std::uint8_t>(v));
        } else if (v >= -128) {
            put(0xd0);
            put(static_cast<std::uint8_t>(v));
        } else if (v >= -32768) {
            put(0xd1);
            be(static_cast<std::uint16_t>(v));
        } else if (v >= -2147483648LL) {
            put(0xd2);
            be(static_cast<std::uint32_t>(v));
        } else {
            put(0xd3);
            be(static_cast<std::uint64_t>(v));
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
    [[nodiscard]] std::vector<std::uint8_t> release() noexcept { return std::move(buf_); }
    void clear() noexcept { buf_.clear(); }

private:
    void put(std::uint8_t b) { buf_.push_back(b); }

    template <typename T>
    void be(T v) {
        for (int i = static_cast<int>(sizeof(T)) - 1; i >= 0; --i) {
            put(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }

    void collectionHeader(std::size_t n, std::uint8_t fix, std::uint8_t p16, std::uint8_t p32) {
        if (n < 16) {
            put(static_cast<std::uint8_t>(fix | n));
        } else if (n <= 0xFFFF) {
            put(p16);
            be(static_cast<std::uint16_t>(n));
        } else {
            put(p32);
            be(static_cast<std::uint32_t>(n));
        }
    }

    std::vector<std::uint8_t> buf_;
};

}  // namespace hl
