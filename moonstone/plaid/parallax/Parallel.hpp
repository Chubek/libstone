#pragma once
#include "../loom/Scheduler.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace moonstone::plaid::parallax {
// Explicit parallel algorithm. The programmer is responsible for proving that
// iteration effects commute and shared data are synchronized or disjoint.
template <class Fn>
void parallel_for(loom::WorkStealingPool &pool, std::size_t begin, std::size_t end,
                  std::size_t grain, Fn fn) {
    if (!grain)
        throw std::invalid_argument("grain must be positive");
    if (begin > end)
        throw std::invalid_argument("inverted iteration range");
    loom::TaskScope scope(pool);
    for (std::size_t from = begin; from < end;) {
        const auto size = std::min(grain, end - from);
        const auto to = from + size;
        scope.spawn([=](const loom::CancellationToken &cancel) {
            for (auto i = from; i < to && !cancel.requested(); ++i)
                fn(i);
        });
        from = to;
    }
    scope.join();
}

// Parallel reduction is valid only if 'combine' is associative under the
// selected language semantics. Floating-point reassociation changes results.
template <class T, class Transform, class Combine>
T parallel_reduce(loom::WorkStealingPool &pool, std::size_t begin, std::size_t end,
                  std::size_t grain, T identity, Transform transform, Combine combine) {
    if (!grain || begin > end)
        throw std::invalid_argument("invalid reduce range");
    std::vector<std::future<T>> jobs;
    // Executor futures do not join on destruction. Keep captures alive until
    // every admitted job finishes, including failure during later admission,
    // vector growth, capture copying, or result initialization.
    struct JoinJobs {
        std::vector<std::future<T>> &jobs;
        ~JoinJobs() {
            for (auto &job : jobs)
                if (job.valid())
                    job.wait();
        }
    } joining{jobs};
    for (std::size_t from = begin; from < end;) {
        const auto to = from + std::min(grain, end - from);
        jobs.emplace_back(); // Allocate tracking storage before admission.
        jobs.back() = pool.submit([=] {
            T accum = identity;
            for (auto i = from; i < to; ++i)
                accum = combine(std::move(accum), transform(i));
            return accum;
        });
        from = to;
    }
    T result = std::move(identity);
    std::exception_ptr first;
    for (auto &job : jobs) {
        try {
            result = combine(std::move(result), job.get());
        } catch (...) {
            if (!first)
                first = std::current_exception();
        }
    }
    if (first)
        std::rethrow_exception(first);
    return result;
}

// Safe portable lane grouping; this is *not* a native instruction vectorizer.
template <class T, std::size_t Width, class Fn>
void lane_map(const T *input, T *output, std::size_t count, Fn transform) {
    static_assert(Width > 0);
    if (count && (!input || !output))
        throw std::invalid_argument("null lane buffers");
    for (std::size_t start = 0; start < count;) {
        const auto active = std::min(Width, count - start);
        for (std::size_t lane = 0; lane < active; ++lane)
            output[start + lane] = transform(input[start + lane]);
        start += active;
    }
}
} // namespace moonstone::plaid::parallax
