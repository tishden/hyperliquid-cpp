#include "hl/core/Log.h"

#include <atomic>
#include <cstdio>

namespace hl {

namespace {

void defaultSink(LogLevel level, std::string_view message, void* /*userData*/) {
    static constexpr const char* kNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF"};
    std::fprintf(stderr, "[hl][%s] %.*s\n", kNames[static_cast<int>(level)], static_cast<int>(message.size()),
                 message.data());
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
