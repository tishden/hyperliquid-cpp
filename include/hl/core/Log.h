// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string_view>

namespace hl {

/// Log severity.
enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

/**
 * @brief Log sink signature. Receives a fully formatted, NUL-terminated line
 *        (no trailing newline). Must be thread-safe if the library is used
 *        from several threads.
 */
using LogSink = void (*)(LogLevel level, std::string_view message, void* userData);

/**
 * @brief Install a process-wide log sink.
 *
 * The default sink writes `[hl][LEVEL] message` to stderr at Info and above.
 * Pass `nullptr` to restore the default. The library logs only on the control
 * path (connects, reconnects, errors) — never per market-data message.
 */
void setLogSink(LogSink sink, void* userData = nullptr) noexcept;

/// Minimum level forwarded to the sink (default: Info).
void setLogLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel logLevel() noexcept;

/// printf-style logging entry point used by the library.
void logf(LogLevel level, const char* fmt, ...) noexcept
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace hl
