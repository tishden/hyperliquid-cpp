// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#include "hl/net/HttpCodec.h"

#include <cctype>
#include <charconv>

namespace hl {

namespace {

bool iequalsPrefix(std::string_view line, std::string_view name) {
    if (line.size() < name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool containsTokenCi(std::string_view value, std::string_view token) {
    std::string lower(value.size(), '\0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
    }
    return lower.find(token) != std::string::npos;
}

}  // namespace

void HttpResponseParser::reset() {
    phase_ = Phase::Head;
    head_.clear();
    line_.clear();
    remaining_ = 0;
    response_ = HttpResponse{};
    error_.clear();
}

HttpResponseParser::Status HttpResponseParser::parseHead() {
    std::string_view head{head_};
    std::size_t eol = head.find("\r\n");
    const std::string_view statusLine = head.substr(0, eol);
    if (statusLine.rfind("HTTP/1.", 0) != 0 || statusLine.size() < 12) {
        error_ = "malformed status line";
        return Status::Error;
    }
    int code = 0;
    const auto res = std::from_chars(statusLine.data() + 9, statusLine.data() + 12, code);
    if (res.ec != std::errc{}) {
        error_ = "malformed status code";
        return Status::Error;
    }
    response_.status = code;
    response_.keepAlive = statusLine.rfind("HTTP/1.0", 0) != 0;

    bool chunked = false;
    bool haveLength = false;
    std::size_t length = 0;
    std::size_t pos = eol + 2;
    while (pos < head.size()) {
        eol = head.find("\r\n", pos);
        if (eol == std::string_view::npos || eol == pos) {
            break;
        }
        const std::string_view line = head.substr(pos, eol - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view value = trim(line.substr(colon + 1));
            if (iequalsPrefix(line, "content-length:")) {
                std::uint64_t v = 0;
                if (std::from_chars(value.data(), value.data() + value.size(), v).ec != std::errc{}) {
                    error_ = "bad content-length";
                    return Status::Error;
                }
                length = static_cast<std::size_t>(v);
                haveLength = true;
            } else if (iequalsPrefix(line, "transfer-encoding:")) {
                chunked = containsTokenCi(value, "chunked");
            } else if (iequalsPrefix(line, "retry-after:")) {
                std::uint64_t seconds = 0;
                if (std::from_chars(value.data(), value.data() + value.size(), seconds).ec == std::errc{}) {
                    response_.retryAfterSeconds = static_cast<int>(seconds);
                }
            } else if (iequalsPrefix(line, "connection:")) {
                if (containsTokenCi(value, "close")) {
                    response_.keepAlive = false;
                } else if (containsTokenCi(value, "keep-alive")) {
                    response_.keepAlive = true;
                }
            }
        }
        pos = eol + 2;
    }
    const bool noBody = code == 204 || code == 304 || (code >= 100 && code < 200);
    if (noBody) {
        phase_ = Phase::Done;
    } else if (chunked) {
        phase_ = Phase::ChunkSize;
    } else if (haveLength) {
        remaining_ = length;
        response_.body.reserve(length);
        phase_ = length == 0 ? Phase::Done : Phase::FixedBody;
    } else {
        response_.keepAlive = false;
        phase_ = Phase::UntilClose;
    }
    return Status::NeedMore;
}

HttpResponseParser::Status HttpResponseParser::feed(const char* data, std::size_t len, std::size_t& consumed) {
    std::size_t i = 0;
    while (i < len && phase_ != Phase::Done) {
        switch (phase_) {
            case Phase::Head: {
                // Accumulate until the blank line; scan only the newly appended region.
                const std::size_t before = head_.size();
                const std::string_view chunk{data + i, len - i};
                head_.append(chunk);
                const std::size_t searchFrom = before >= 3 ? before - 3 : 0;
                const std::size_t end = head_.find("\r\n\r\n", searchFrom);
                if (end == std::string::npos) {
                    if (head_.size() > 64 * 1024) {
                        error_ = "response head too large";
                        return Status::Error;
                    }
                    i = len;
                    break;
                }
                const std::size_t headLen = end + 4;
                i += headLen - before;
                head_.resize(headLen);
                if (parseHead() == Status::Error) {
                    return Status::Error;
                }
                break;
            }
            case Phase::FixedBody: {
                const std::size_t take = std::min(remaining_, len - i);
                response_.body.append(data + i, take);
                remaining_ -= take;
                i += take;
                if (remaining_ == 0) {
                    phase_ = Phase::Done;
                }
                break;
            }
            case Phase::ChunkSize:
            case Phase::Trailer: {
                const char c = data[i++];
                if (c != '\n') {
                    line_.push_back(c);
                    if (line_.size() > 1024) {
                        error_ = "chunk header too long";
                        return Status::Error;
                    }
                    break;
                }
                std::string_view line = trim(line_);
                if (phase_ == Phase::Trailer) {
                    if (line.empty()) {
                        phase_ = Phase::Done;
                    }
                    line_.clear();
                    break;
                }
                if (const auto semi = line.find(';'); semi != std::string_view::npos) {
                    line = line.substr(0, semi);
                }
                std::uint64_t size = 0;
                if (std::from_chars(line.data(), line.data() + line.size(), size, 16).ec != std::errc{}) {
                    error_ = "bad chunk size";
                    return Status::Error;
                }
                line_.clear();
                if (size == 0) {
                    phase_ = Phase::Trailer;
                } else {
                    remaining_ = static_cast<std::size_t>(size);
                    phase_ = Phase::ChunkData;
                }
                break;
            }
            case Phase::ChunkData: {
                const std::size_t take = std::min(remaining_, len - i);
                response_.body.append(data + i, take);
                remaining_ -= take;
                i += take;
                if (remaining_ == 0) {
                    phase_ = Phase::ChunkDataCrlf;
                    remaining_ = 2;
                }
                break;
            }
            case Phase::ChunkDataCrlf: {
                ++i;
                if (--remaining_ == 0) {
                    phase_ = Phase::ChunkSize;
                }
                break;
            }
            case Phase::UntilClose:
                response_.body.append(data + i, len - i);
                i = len;
                break;
            case Phase::Done:
                break;
        }
    }
    consumed = i;
    return phase_ == Phase::Done ? Status::Complete : Status::NeedMore;
}

HttpResponseParser::Status HttpResponseParser::finishOnClose() {
    if (phase_ == Phase::UntilClose || phase_ == Phase::Done) {
        phase_ = Phase::Done;
        return Status::Complete;
    }
    error_ = "connection closed mid-response";
    return Status::Error;
}

}  // namespace hl
