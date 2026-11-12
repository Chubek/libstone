#include "../loom/Scheduler.hpp"
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <future>
#include <random>
#include <thread>
#include <vector>

using namespace moonstone::plaid::loom;
using namespace std::chrono_literals;

static void bounded_admission() {
    WorkStealingPool pool(1, 1);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto active = pool.submit([&] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    auto queued = pool.submit([] { return 42; });
    bool rejected = false;
    try {
        pool.submit([] { assert(false); });
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
    TaskScope scope(pool);
    rejected = false;
    try {
        scope.spawn([](const CancellationToken &) { assert(false); });
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
    scope.join(); // A failed admission must not leave an invalid future.
    release.set_value();
    active.get();
    assert(queued.get() == 42);
    scope.spawn([](const CancellationToken &) {});
    scope.join();
}

static void worker_stop_and_failure() {
    WorkStealingPool pool(2);
    auto self_stop = pool.submit([&] {
        try {
            pool.stop();
        } catch (const std::logic_error &) {
            return true;
        }
        return false;
    });
    assert(self_stop.get());
    TaskScope scope(pool);
    scope.spawn([](const CancellationToken &) { throw std::logic_error("child failed"); });
    bool failed = false;
    try {
        scope.join();
    } catch (const std::logic_error &) {
        failed = true;
    }
    assert(failed);
    auto stop = pool.submit([&] { pool.request_stop(); });
    stop.get();
    pool.stop();
    pool.stop();
    bool rejected = false;
    try {
        pool.submit([] {});
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
}

static void racing_stop() {
    for (unsigned round = 0; round < 80; ++round) {
        WorkStealingPool pool(3);
        std::atomic<int> accepted{0}, executed{0};
        std::barrier start(7);
        std::vector<std::thread> producers;
        for (unsigned p = 0; p < 4; ++p)
            producers.emplace_back([&, p] {
                std::minstd_rand random(1234 + round * 4 + p);
                start.arrive_and_wait();
                for (int i = 0; i < 100; ++i) {
                    try {
                        pool.submit([&, fail = (i % 7 == 0)] {
                            executed.fetch_add(1, std::memory_order_relaxed);
                            if (fail)
                                throw std::runtime_error("expected task failure");
                        });
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    } catch (const std::runtime_error &) {
                        break;
                    }
                    if (random() % 3 == 0)
                        std::this_thread::yield();
                }
            });
        std::thread stop1([&] {
            start.arrive_and_wait();
            pool.stop();
        });
        std::thread stop2([&] {
            start.arrive_and_wait();
            pool.stop();
        });
        start.arrive_and_wait();
        for (auto &producer : producers)
            producer.join();
        stop1.join();
        stop2.join();
        assert(accepted.load() == executed.load());
    }
}

static void child_failure_cancels_siblings() {
    WorkStealingPool pool(2);
    TaskScope scope(pool);
    std::promise<void> entered;
    scope.spawn([&](const CancellationToken &token) {
        entered.set_value();
        while (!token.requested())
            std::this_thread::yield();
    });
    entered.get_future().wait();
    scope.spawn([](const CancellationToken &) { throw std::runtime_error("cancel siblings"); });
    bool failed = false;
    try {
        scope.join();
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);
}

static void idle_wakeup() {
    WorkStealingPool pool(4);
    for (int i = 0; i < 1000; ++i) {
        auto done = pool.submit([i] { return i; });
        assert(done.wait_for(5s) == std::future_status::ready);
        assert(done.get() == i);
        if (i % 10 == 0)
            std::this_thread::sleep_for(20us);
    }
}

int main() {
    bounded_admission();
    worker_stop_and_failure();
    child_failure_cancels_siblings();
    racing_stop();
    idle_wakeup();
}
