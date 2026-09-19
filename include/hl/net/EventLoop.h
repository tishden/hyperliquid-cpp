// SPDX-License-Identifier: LicenseRef-hyperliquid-cpp
// Copyright (c) 2026 Denis Tishkov <denis8825@ya.ru>. All rights reserved. See LICENSE.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace hl {

/// Receives readiness notifications for a registered file descriptor.
class IoHandler {
public:
    /// @param events epoll event mask (EPOLLIN / EPOLLOUT / EPOLLERR / EPOLLHUP).
    virtual void onIoEvent(std::uint32_t events) = 0;

protected:
    ~IoHandler() = default;
};

/**
 * @brief Single-threaded epoll reactor with millisecond timers.
 *
 * Every client in this library (MarketDataClient, ExchangeClient, InfoClient)
 * is driven by an EventLoop that the application owns. Run it on a dedicated
 * thread with `run()`, or embed it in an existing loop by calling
 * `runOnce(0)` periodically (busy-poll) or polling `fd()` for readability.
 *
 * All callbacks — I/O, timers, posted tasks — execute on the thread that calls
 * `run`/`runOnce`. The loop is **not** thread-safe except for `wake()` and
 * `stop()`; hand work to it from other threads with `postThreadSafe()`.
 */
class EventLoop {
public:
    using TimerId = std::uint64_t;
    using Task = std::function<void()>;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// Register @p fd for the given epoll @p events. Replaces an existing registration.
    void add(int fd, std::uint32_t events, IoHandler* handler);
    /// Change the event mask of a registered fd.
    void modify(int fd, std::uint32_t events);
    /// Unregister @p fd (safe to call from inside its own callback).
    void remove(int fd);

    /// Run @p task once after @p delayMs milliseconds. Returns an id for cancelTimer.
    TimerId addTimer(std::int64_t delayMs, Task task);
    /// Cancel a pending timer (no-op if it already fired).
    void cancelTimer(TimerId id);

    /**
     * @brief Queue @p task to run on the loop thread (thread-safe; wakes the loop).
     *
     * The loop must outlive every thread that can call this; a task posted to a destroyed loop is
     * undefined behaviour. Tasks queued but not yet run are dropped when the loop is destroyed.
     */
    void postThreadSafe(Task task);

    /**
     * @brief Wait up to @p maxWaitMs for I/O, then dispatch I/O, due timers and posted tasks.
     * @param maxWaitMs  0 = non-blocking poll, -1 = wait until something happens.
     * @return number of I/O events dispatched (timers and posted tasks are not counted).
     */
    int runOnce(int maxWaitMs = -1);

    /// Dispatch until `stop()` is called. Returns immediately if a stop was already requested;
    /// the request is consumed when `run()` returns, so the loop can be run again afterwards.
    void run();
    /// Ask `run()` to return (thread-safe, async-signal-safe). May be called before `run()`.
    void stop();
    /// True while a stop request is pending (set by `stop()`, cleared when `run()` returns).
    [[nodiscard]] bool stopped() const noexcept;

    /// The epoll descriptor, for integration into an outer poller.
    [[nodiscard]] int fd() const noexcept { return epollFd_; }

    /// Monotonic milliseconds (CLOCK_MONOTONIC).
    [[nodiscard]] static std::int64_t nowMs() noexcept;
    /// Wall-clock milliseconds since the Unix epoch.
    [[nodiscard]] static std::int64_t wallClockMs() noexcept;

private:
    struct Registration {
        IoHandler* handler{nullptr};
        std::uint32_t generation{0};
    };
    struct Timer {
        TimerId id;
        Task task;
    };

    void drainWakeups();
    void runTimers();
    void runPosted();
    [[nodiscard]] int nextTimeoutMs(int maxWaitMs) const;

    int epollFd_{-1};
    int wakeFd_{-1};
    std::uint32_t nextGeneration_{1};
    std::unordered_map<int, Registration> registrations_;
    std::multimap<std::int64_t, Timer> timers_;
    std::unordered_map<TimerId, std::multimap<std::int64_t, Timer>::iterator> timerIndex_;
    TimerId nextTimerId_{1};
    struct Shared;
    Shared* shared_{nullptr};
};

}  // namespace hl
