#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "hl/net/EventLoop.h"

TEST(EventLoop, TimersFireInOrderAndCanBeCanceled) {
    hl::EventLoop loop;
    std::vector<int> order;
    loop.addTimer(30, [&] { order.push_back(3); });
    loop.addTimer(10, [&] { order.push_back(1); });
    const auto id = loop.addTimer(20, [&] { order.push_back(2); });
    loop.addTimer(40, [&] { loop.stop(); });
    loop.cancelTimer(id);
    loop.run();
    EXPECT_EQ(order, (std::vector<int>{1, 3}));
}

TEST(EventLoop, PostThreadSafeWakesLoop) {
    hl::EventLoop loop;
    int value = 0;
    std::thread t([&] {
        loop.postThreadSafe([&] {
            value = 42;
            loop.stop();
        });
    });
    loop.run();
    t.join();
    EXPECT_EQ(value, 42);
}

TEST(EventLoop, DispatchesIoAndSkipsRemovedHandlers) {
    struct Handler final : hl::IoHandler {
        int fd{-1};
        int calls{0};
        hl::EventLoop* loop{nullptr};
        Handler* other{nullptr};
        void onIoEvent(std::uint32_t) override {
            ++calls;
            std::uint64_t v = 0;
            [[maybe_unused]] auto n = ::read(fd, &v, sizeof(v));
            if (other != nullptr) {
                loop->remove(other->fd);  // removing a handler whose event is already queued
            }
        }
    };
    hl::EventLoop loop;
    Handler a;
    Handler b;
    a.fd = ::eventfd(1, EFD_NONBLOCK);
    b.fd = ::eventfd(1, EFD_NONBLOCK);
    a.loop = b.loop = &loop;
    a.other = &b;
    b.other = &a;
    loop.add(a.fd, EPOLLIN, &a);
    loop.add(b.fd, EPOLLIN, &b);
    loop.runOnce(100);
    EXPECT_EQ(a.calls + b.calls, 1);
    ::close(a.fd);
    ::close(b.fd);
}

TEST(EventLoop, RunOnceHonoursTimeout) {
    hl::EventLoop loop;
    const auto t0 = hl::EventLoop::nowMs();
    loop.runOnce(20);
    EXPECT_GE(hl::EventLoop::nowMs() - t0, 15);
    EXPECT_GT(hl::EventLoop::wallClockMs(), 1'600'000'000'000);
}

TEST(EventLoop, StopBeforeRunIsHonouredAndConsumed) {
    hl::EventLoop loop;
    loop.stop();
    EXPECT_TRUE(loop.stopped());
    loop.run();  // must return immediately
    EXPECT_FALSE(loop.stopped());
    int fired = 0;
    loop.addTimer(5, [&] {
        ++fired;
        loop.stop();
    });
    loop.run();  // runs again until the timer stops it
    EXPECT_EQ(fired, 1);
}
