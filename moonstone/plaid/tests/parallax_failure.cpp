#include "../parallax/Parallel.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <numeric>
#include <vector>

using namespace moonstone::plaid;
using namespace std::chrono_literals;

struct CopyState {
    std::atomic<int> copies{0};
    std::promise<void> second_copy;
};

struct Transform {
    std::shared_ptr<CopyState> state;
    std::atomic<int> *completed;
    Transform(std::shared_ptr<CopyState> s, std::atomic<int> &count)
        : state(std::move(s)), completed(&count) {}
    Transform(const Transform &other) : state(other.state), completed(other.completed) {
        if (state->copies.fetch_add(1) == 1)
            state->second_copy.set_value();
    }
    Transform(Transform &&) noexcept = default;
    int operator()(std::size_t) const {
        completed->fetch_add(1);
        return 1;
    }
};

static void rejected_submission_joins() {
    loom::WorkStealingPool pool(1, 1);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto active = pool.submit([&] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    auto state = std::make_shared<CopyState>();
    std::atomic<int> completed{0};
    auto result = std::async(std::launch::async, [&] {
        try {
            parallax::parallel_reduce(pool, 0, 10, 1, 0, Transform(state, completed),
                                      std::plus<int>{});
        } catch (const std::runtime_error &) {
            return true;
        }
        return false;
    });
    state->second_copy.get_future().wait();
    // The first reduction job is queued behind active. Rejection of the
    // second must not allow the reduction to return while the first is live.
    assert(result.wait_for(20ms) == std::future_status::timeout);
    release.set_value();
    active.get();
    assert(result.get() && completed.load() == 1);
}

static void scalar_differential() {
    loom::WorkStealingPool pool(3);
    for (std::size_t count : {0u, 1u, 2u, 7u, 31u, 129u}) {
        for (std::size_t grain : {1u, 3u, 64u, 200u}) {
            std::vector<int> scalar(count), parallel(count);
            for (std::size_t i = 0; i < count; ++i)
                scalar[i] = int(i * i + 3);
            parallax::parallel_for(pool, 0, count, grain,
                                   [&](std::size_t i) { parallel[i] = int(i * i + 3); });
            assert(parallel == scalar);
            auto sum = parallax::parallel_reduce(
                pool, 0, count, grain, 0, [&](std::size_t i) { return parallel[i]; },
                std::plus<int>{});
            assert(sum == std::accumulate(scalar.begin(), scalar.end(), 0));
        }
    }
}

int main() {
    rejected_submission_joins();
    scalar_differential();
}
