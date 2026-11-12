#include "../chorus/Actor.hpp"
#include <atomic>
#include <barrier>
#include <cassert>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using namespace moonstone::plaid;

static void bounded_mailbox_and_self_wait() {
    loom::WorkStealingPool pool(2);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<int> processed{0};
    auto actor = chorus::Actor<int>::create(
        pool,
        [&](int value) {
            if (value == 1) {
                entered.set_value();
                gate.wait();
            }
            processed.fetch_add(1);
        },
        1);
    assert(actor->send(1));
    entered.get_future().wait();
    assert(actor->send(2) && !actor->send(3));
    actor->close();
    assert(!actor->send(4));
    release.set_value();
    actor->close_and_wait();
    assert(processed.load() == 2);

    std::shared_ptr<chorus::Actor<int>> self;
    self = chorus::Actor<int>::create(pool, [&](int) {
        bool rejected = false;
        try {
            self->close_and_wait();
        } catch (const std::logic_error &) {
            rejected = true;
        }
        assert(rejected);
        self->close();
    });
    assert(self->send(0));
    self->close_and_wait();
}

static void retained_lifetime_and_rejected_admission() {
    loom::WorkStealingPool pool(1);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto busy = pool.submit([&] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    std::atomic<int> observed{0};
    auto actor = chorus::Actor<std::unique_ptr<int>>::create(
        pool, [&](auto value) { observed.store(*value); });
    std::weak_ptr<chorus::Actor<std::unique_ptr<int>>> weak = actor;
    assert(actor->send(std::make_unique<int>(42)));
    actor.reset();
    assert(!weak.expired());
    release.set_value();
    busy.get();
    pool.stop();
    assert(observed.load() == 42 && weak.expired());
    auto stopped = chorus::Actor<int>::create(pool, [](int) { assert(false); });
    bool rejected = false;
    try {
        stopped->send(1);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
    stopped->close_and_wait(); // Rejection must not strand scheduled_ true.
}

static void concurrent_close_and_failure() {
    for (int round = 0; round < 80; ++round) {
        loom::WorkStealingPool pool(3);
        std::atomic<int> accepted{0}, processed{0}, active{0};
        auto actor = chorus::Actor<int>::create(pool, [&](int) {
            assert(active.fetch_add(1) == 0);
            processed.fetch_add(1);
            std::this_thread::yield();
            assert(active.fetch_sub(1) == 1);
        });
        std::barrier start(7);
        std::vector<std::thread> threads;
        for (int p = 0; p < 4; ++p)
            threads.emplace_back([&] {
                start.arrive_and_wait();
                for (int i = 0; i < 100; ++i) {
                    if (!actor->send(i))
                        break;
                    accepted.fetch_add(1);
                    std::this_thread::yield();
                }
            });
        for (int c = 0; c < 2; ++c)
            threads.emplace_back([&] {
                start.arrive_and_wait();
                actor->close_and_wait();
            });
        start.arrive_and_wait();
        for (auto &thread : threads)
            thread.join();
        assert(accepted.load() == processed.load());
        auto failed =
            chorus::Actor<int>::create(pool, [](int) { throw std::runtime_error("failure"); });
        assert(failed->send(1));
        for (int i = 0; i < 2; ++i) {
            bool reported = false;
            try {
                failed->close_and_wait();
            } catch (const std::runtime_error &) {
                reported = true;
            }
            assert(reported && !failed->send(2));
        }
    }
}

int main() {
    bounded_mailbox_and_self_wait();
    retained_lifetime_and_rejected_admission();
    concurrent_close_and_failure();
}
