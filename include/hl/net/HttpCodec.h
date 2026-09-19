// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hl {

/// A complete HTTP response.
struct HttpResponse {
    int status{0};
    std::string body{};
    bool keepAlive{true};       ///< false if the server sent `Connection: close`
    int retryAfterSeconds{0};   ///< `Retry-After` header (0 if absent); set on 429 / 503
};

/**
 * @brief Incremental HTTP/1.1 response parser (status line, headers,
 *        Content-Length / chunked / read-until-close bodies).
 */
class HttpResponseParser {
public:
    enum class Status : std::uint8_t { NeedMore, Complete, Error };

    /**
     * @brief Consume bytes.
     * @param consumed receives how many bytes of @p data belong to this response
     *        (the rest belong to a following response on the same connection).
     */
    Status feed(const char* data, std::size_t len, std::size_t& consumed);

    /// Signal EOF: completes a read-until-close body. Returns Complete or Error.
    Status finishOnClose();

    [[nodiscard]] const HttpResponse& response() const noexcept { return response_; }
    [[nodiscard]] HttpResponse takeResponse() noexcept { return std::move(response_); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    void reset();

private:
    enum class Phase : std::uint8_t { Head, FixedBody, ChunkSize, ChunkData, ChunkDataCrlf, Trailer, UntilClose, Done };

    Status parseHead();

    Phase phase_{Phase::Head};
    std::string head_;
    std::string line_;
    std::size_t remaining_{0};
    HttpResponse response_;
    std::string error_;
};

}  // namespace hl
