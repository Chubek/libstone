#pragma once

// Private definition of RepetitionScheduler::Impl. The public header keeps the
// type incomplete, so every translation unit that constructs or destroys a
// scheduler must see this file (Execution.hpp documents the same pimpl style).
#include "mahak/Execution.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace moonstone::mahak {
inline namespace v1 {

struct RepetitionScheduler::Impl {
    ScheduleOptions options;
    Rng rng;
    std::uint32_t warmup_completed = 0;
    std::uint32_t measured = 0;
    std::uint64_t elapsed_total_ns = 0;
    std::vector<Observation> observations;
    bool stop_requested = false;

    explicit Impl(ScheduleOptions value)
        : options(std::move(value)), rng(options.seed != 0 ? options.seed : options.policy.seed) {}

    std::uint32_t ceiling() const {
        if (options.hard_max_repetitions != 0)
            return std::max(options.hard_max_repetitions, options.policy.repetitions);
        const std::uint32_t configured = std::max(options.policy.repetitions,
                                                  options.policy.max_repetitions);
        return std::max<std::uint32_t>(configured, 1);
    }
};

} // namespace v1
} // namespace moonstone::mahak
