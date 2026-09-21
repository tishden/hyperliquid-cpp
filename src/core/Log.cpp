// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace hl {

namespace {

void defaultSink(LogLevel level, std::string_view message, void* /*userData*/) {
    static constexpr const char* kNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF"};
    // A trading log without a clock cannot be matched against the venue's own record of what
    // happened, which is the first thing anyone does when an order behaves unexpectedly.
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::floor<std::chrono::seconds>(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms));
    std::fprintf(stderr, "%s [hl][%s] %.*s\n", stamp, kNames[static_cast<int>(level)],
                 static_cast<int>(message.size()), message.data());
}

std::atomic<LogSink> gSink{&defaultSink};
std::atomic<void*> gUserData{nullptr};
std::atomic<LogLevel> gLevel{LogLevel::Info};

}  // namespace

void setLogSink(LogSink sink, void* userData) noexcept {
    gUserData.store(userData);
    gSink.store(sink != nullptr ? sink : &defaultSink);
}

void setLogLevel(LogLevel level) noexcept { gLevel.store(level); }

LogLevel logLevel() noexcept { return gLevel.load(); }

void logf(LogLevel level, const char* fmt, ...) noexcept {
    if (level < gLevel.load(std::memory_order_relaxed) || level == LogLevel::Off) {
        return;
    }
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    const auto len = static_cast<std::size_t>(n) < sizeof(buf) ? static_cast<std::size_t>(n) : sizeof(buf) - 1;
    gSink.load()(level, std::string_view{buf, len}, gUserData.load());
}

}  // namespace hl
