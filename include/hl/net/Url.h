// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hl {

/// Parsed `scheme://host[:port][/path]` URL (http, https, ws, wss).
struct Url {
    std::string scheme;
    std::string host;
    std::uint16_t port{0};
    std::string path{"/"};
    bool secure{false};

    [[nodiscard]] static std::optional<Url> parse(std::string_view text);
};

}  // namespace hl
