#include "hl/net/Url.h"

#include <charconv>

namespace hl {

std::optional<Url> Url::parse(std::string_view text) {
    Url url;
    const auto schemeEnd = text.find("://");
    if (schemeEnd == std::string_view::npos) {
        return std::nullopt;
    }
    url.scheme = std::string{text.substr(0, schemeEnd)};
    std::uint16_t defaultPort = 0;
    if (url.scheme == "https" || url.scheme == "wss") {
        url.secure = true;
        defaultPort = 443;
    } else if (url.scheme == "http" || url.scheme == "ws") {
        defaultPort = 80;
    } else {
        return std::nullopt;
    }
    std::string_view rest = text.substr(schemeEnd + 3);
    const auto pathStart = rest.find('/');
    std::string_view authority = rest.substr(0, pathStart);
    if (pathStart != std::string_view::npos) {
        url.path = std::string{rest.substr(pathStart)};
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        unsigned port = 0;
        const auto portText = authority.substr(colon + 1);
        const auto res = std::from_chars(portText.data(), portText.data() + portText.size(), port);
        if (res.ec != std::errc{} || port == 0 || port > 65535) {
            return std::nullopt;
        }
        url.port = static_cast<std::uint16_t>(port);
        authority = authority.substr(0, colon);
    } else {
        url.port = defaultPort;
    }
    if (authority.empty()) {
        return std::nullopt;
    }
    url.host = std::string{authority};
    return url;
}

}  // namespace hl
