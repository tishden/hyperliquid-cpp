// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#include "hl/net/EventLoop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>

#include "hl/core/Log.h"

namespace hl {

struct EventLoop::Shared {
    std::mutex mutex;
    std::vector<Task> posted;
    std::atomic<bool> stop{false};
};

namespace {

constexpr std::uint64_t kWakeTag = ~0ULL;

std::uint64_t encode(int fd, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint32_t>(fd);
}

}  // namespace

EventLoop::EventLoop() : shared_(new Shared) {
    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wakeFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epollFd_ < 0 || wakeFd_ < 0) {
        throw std::runtime_error(std::string{"hl::EventLoop: "} + std::strerror(errno));
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kWakeTag;
    ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &ev);
}

EventLoop::~EventLoop() {
    if (wakeFd_ >= 0) {
        ::close(wakeFd_);
    }
    if (epollFd_ >= 0) {
        ::close(epollFd_);
    }
    delete shared_;
}

void EventLoop::add(int fd, std::uint32_t events, IoHandler* handler) {
    const std::uint32_t gen = nextGeneration_++;
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = encode(fd, gen);
    const bool existed = registrations_.count(fd) != 0;
    registrations_[fd] = Registration{handler, gen};
    if (::epoll_ctl(epollFd_, existed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &ev) != 0) {
        logf(LogLevel::Error, "epoll_ctl(add fd=%d): %s", fd, std::strerror(errno));
    }
}

void EventLoop::modify(int fd, std::uint32_t events) {
    auto it = registrations_.find(fd);
    if (it == registrations_.end()) {
        return;
    }
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = encode(fd, it->second.generation);
    if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
        logf(LogLevel::Error, "epoll_ctl(mod fd=%d): %s", fd, std::strerror(errno));
    }
}

void EventLoop::remove(int fd) {
    if (registrations_.erase(fd) != 0) {
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    }
}

EventLoop::TimerId EventLoop::addTimer(std::int64_t delayMs, Task task) {
    const TimerId id = nextTimerId_++;
    auto it = timers_.emplace(nowMs() + (delayMs < 0 ? 0 : delayMs), Timer{id, std::move(task)});
    timerIndex_.emplace(id, it);
    return id;
}

void EventLoop::cancelTimer(TimerId id) {
    auto it = timerIndex_.find(id);
    if (it != timerIndex_.end()) {
        timers_.erase(it->second);
        timerIndex_.erase(it);
    }
}

void EventLoop::postThreadSafe(Task task) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->posted.push_back(std::move(task));
    }
    const std::uint64_t one = 1;
    [[maybe_unused]] const auto n = ::write(wakeFd_, &one, sizeof(one));
}

void EventLoop::stop() {
    shared_->stop.store(true);
    const std::uint64_t one = 1;
    [[maybe_unused]] const auto n = ::write(wakeFd_, &one, sizeof(one));
}

bool EventLoop::stopped() const noexcept { return shared_->stop.load(); }

std::int64_t EventLoop::nowMs() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

std::int64_t EventLoop::wallClockMs() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

void EventLoop::drainWakeups() {
    std::uint64_t value = 0;
    while (::read(wakeFd_, &value, sizeof(value)) > 0) {
    }
}

int EventLoop::nextTimeoutMs(int maxWaitMs) const {
    if (timers_.empty()) {
        return maxWaitMs;
    }
    const std::int64_t untilNext = timers_.begin()->first - nowMs();
    const int timerWait = untilNext <= 0 ? 0 : static_cast<int>(untilNext);
    if (maxWaitMs < 0) {
        return timerWait;
    }
    return timerWait < maxWaitMs ? timerWait : maxWaitMs;
}

void EventLoop::runTimers() {
    const std::int64_t now = nowMs();
    while (!timers_.empty() && timers_.begin()->first <= now) {
        auto node = timers_.extract(timers_.begin());
        timerIndex_.erase(node.mapped().id);
        node.mapped().task();
    }
}

void EventLoop::runPosted() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        tasks.swap(shared_->posted);
    }
    for (auto& t : tasks) {
        t();
    }
}

int EventLoop::runOnce(int maxWaitMs) {
    epoll_event events[64];
    const int n = ::epoll_wait(epollFd_, events, 64, nextTimeoutMs(maxWaitMs));
    int dispatched = 0;
    for (int i = 0; i < n; ++i) {
        const std::uint64_t tag = events[i].data.u64;
        if (tag == kWakeTag) {
            drainWakeups();
            continue;
        }
        const int fd = static_cast<int>(static_cast<std::uint32_t>(tag));
        const auto gen = static_cast<std::uint32_t>(tag >> 32);
        auto it = registrations_.find(fd);
        // Skip events for fds removed (or re-registered) earlier in this batch.
        if (it == registrations_.end() || it->second.generation != gen) {
            continue;
        }
        it->second.handler->onIoEvent(events[i].events);
        ++dispatched;
    }
    runTimers();
    runPosted();
    return dispatched;
}

void EventLoop::run() {
    // A stop() requested before run() (e.g. a signal during start-up) is honoured: run() returns
    // immediately. The request is consumed on return so the loop can be run again.
    while (!shared_->stop.load(std::memory_order_relaxed)) {
        runOnce(-1);
    }
    shared_->stop.store(false);
}

}  // namespace hl
