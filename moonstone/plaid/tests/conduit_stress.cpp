#include "../conduit/Select.hpp"
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

using namespace moonstone::plaid::conduit;
using namespace std::chrono_literals;

static void selection_lifetime() {
    Channel<int> a(2), b(2), rendezvous(0);
    for (int i = 0; i < 2000; ++i) {
        assert(!select_receive<int>({&a, &b, &a}, 0ms));
        assert(a.observer_count() == 0 && b.observer_count() == 0);
    }
    for (auto invalid :
         {std::vector<Channel<int> *>{&a, nullptr}, std::vector<Channel<int> *>{&a, &rendezvous}}) {
        bool rejected = false;
        try {
            (void)select_receive<int>(invalid, 1ms);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected && a.observer_count() == 0);
    }
    auto wake = std::make_shared<WakeSignal>();
    {
        auto outer = a.observe(wake);
        {
            auto inner = a.observe(wake);
            assert(a.observer_count() == 2);
        }
        assert(a.observer_count() == 1); // Unsubscribe only its own slot.
        auto moved = std::move(outer);
        assert(a.observer_count() == 1);
    }
    assert(a.observer_count() == 0);
    std::stop_source already_cancelled;
    already_cancelled.request_stop();
    assert(a.send(42));
    bool cancelled = false;
    try {
        (void)select_receive<int>({&a}, 1s, 0, already_cancelled.get_token());
    } catch (const SelectCancelled &) {
        cancelled = true;
    }
    assert(cancelled && a.receive() == 42 && a.observer_count() == 0);
    assert(b.send(1) && a.send(2));
    auto rotated = select_receive<int>({&a, &b}, 0ms, 1);
    assert(rotated && rotated->index == 1 && rotated->value == 1);
    assert(a.receive() == 2);
}

static void cancelled_and_notified_select() {
    for (int round = 0; round < 150; ++round) {
        Channel<int> c(1);
        std::stop_source cancellation;
        std::barrier start(2);
        auto selected = std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            try {
                auto choice =
                    select_receive<int>({&c, &c}, std::chrono::steady_clock::duration::max(), 0,
                                        cancellation.get_token());
                assert(choice && choice->value == 7);
                return true;
            } catch (const SelectCancelled &) {
                return false;
            }
        });
        start.arrive_and_wait();
        if (round % 2) {
            assert(c.send(7));
        } else {
            cancellation.request_stop();
        }
        assert(selected.wait_for(5s) == std::future_status::ready);
        assert(selected.get() == bool(round % 2));
        assert(c.observer_count() == 0);
    }
}

static void mpmc(std::size_t capacity) {
    constexpr int producers = 4, each = 300;
    Channel<std::unique_ptr<int>> channel(capacity);
    std::vector<std::atomic<int>> seen(producers * each);
    std::vector<std::thread> senders, receivers;
    std::atomic<int> received{0};
    for (unsigned r = 0; r < 4; ++r)
        receivers.emplace_back([&, r] {
            std::minstd_rand random(400 + r);
            while (auto message = channel.receive()) {
                assert(**message >= 0 && **message < producers * each);
                seen[**message].fetch_add(1, std::memory_order_relaxed);
                received.fetch_add(1, std::memory_order_relaxed);
                if (random() % 3 == 0)
                    std::this_thread::yield();
            }
        });
    for (int p = 0; p < producers; ++p)
        senders.emplace_back([&, p] {
            std::minstd_rand random(100 + p);
            for (int i = 0; i < each; ++i) {
                assert(channel.send(std::make_unique<int>(p * each + i)));
                if (random() % 3 == 0)
                    std::this_thread::yield();
            }
        });
    for (auto &sender : senders)
        sender.join();
    channel.close();
    for (auto &receiver : receivers)
        receiver.join();
    assert(received.load() == producers * each);
    for (auto &count : seen)
        assert(count.load() == 1);
}

static void close_race(std::size_t capacity) {
    for (int round = 0; round < 80; ++round) {
        Channel<int> channel(capacity);
        std::atomic<int> accepted{0}, received{0};
        std::barrier start(6);
        std::vector<std::thread> threads;
        for (int p = 0; p < 3; ++p)
            threads.emplace_back([&] {
                start.arrive_and_wait();
                for (int i = 0; i < 100; ++i) {
                    if (!channel.send(i))
                        break;
                    accepted.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            });
        for (int r = 0; r < 2; ++r)
            threads.emplace_back([&] {
                start.arrive_and_wait();
                while (channel.receive())
                    received.fetch_add(1, std::memory_order_relaxed);
            });
        start.arrive_and_wait();
        for (int i = 0; i < round; ++i)
            std::this_thread::yield();
        channel.close();
        channel.close();
        for (auto &thread : threads)
            thread.join();
        assert(accepted.load() == received.load());
        assert(!channel.send(0) && !channel.receive());
    }
}

static void select_close_and_drain() {
    for (int round = 0; round < 200; ++round) {
        Channel<int> c(1);
        std::thread producer([&] {
            assert(c.send(17));
            c.close();
        });
        auto value = select_receive<int>({&c}, 5s);
        assert(value && !value->closed && value->value == 17);
        auto closed = select_receive<int>({&c}, 5s);
        assert(closed && closed->closed && !closed->value);
        producer.join();
        assert(c.observer_count() == 0);
    }
}

int main() {
    selection_lifetime();
    cancelled_and_notified_select();
    for (auto capacity : {0u, 1u, 7u}) {
        mpmc(capacity);
        close_race(capacity);
    }
    select_close_and_drain();
}
