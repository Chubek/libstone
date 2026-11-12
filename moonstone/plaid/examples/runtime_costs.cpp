// Smoke measurement only: no affinity control or statistically robust claims.
#include "../conduit/Channel.hpp"
#include "../conduit/Select.hpp"
#include "../loom/Scheduler.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

using namespace moonstone::plaid;

template <class Fn> long long microseconds(Fn fn) {
    const auto begin = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 begin)
        .count();
}

int main() {
    constexpr int count = 10000;
    std::atomic<int> completed{0};
    const auto direct = microseconds([&] {
        for (int i = 0; i < count; ++i)
            completed.fetch_add(1, std::memory_order_relaxed);
    });
    loom::WorkStealingPool pool(2);
    const auto pooled = microseconds([&] {
        std::vector<std::future<void>> jobs(count);
        for (auto &job : jobs)
            job = pool.submit([&] { completed.fetch_add(1, std::memory_order_relaxed); });
        for (auto &job : jobs)
            job.get();
    });
    const auto wakeups = microseconds([&] {
        for (int i = 0; i < count; ++i)
            pool.submit([] {}).get();
    });
    if (completed.load() != 2 * count)
        return 1;
    std::cout << "operations=" << count << " direct_atomic_us=" << direct
              << " pool_batch_us=" << pooled << " pool_roundtrip_us=" << wakeups << '\n';
    for (auto capacity : {0u, 64u}) {
        conduit::Channel<int> channel(capacity);
        long long sum = 0;
        const auto elapsed = microseconds([&] {
            std::thread receiver([&] {
                while (auto item = channel.receive())
                    sum += *item;
            });
            for (int i = 0; i < count; ++i)
                channel.send(i);
            channel.close();
            receiver.join();
        });
        if (sum != 1LL * count * (count - 1) / 2)
            return 1;
        std::cout << "channel_capacity=" << capacity << " transfer_us=" << elapsed << '\n';
    }
    conduit::Channel<int> empty(1);
    const auto timeouts = microseconds([&] {
        for (int i = 0; i < count; ++i)
            (void)conduit::select_receive<int>({&empty},
                                               std::chrono::steady_clock::duration::zero());
    });
    std::cout << "empty_select_us=" << timeouts << " retained_observers=" << empty.observer_count()
              << '\n';
    return empty.observer_count() == 0 ? 0 : 1;
}
