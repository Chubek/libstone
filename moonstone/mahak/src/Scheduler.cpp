#include "mahak/Execution.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "SchedulerInternal.hpp"

#include "mahak/Statistics.hpp"

namespace moonstone::mahak {
inline namespace v1 {

RepetitionScheduler::RepetitionScheduler(ScheduleOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

std::optional<std::uint64_t> RepetitionScheduler::next() {
    if (!impl_ || impl_->stop_requested)
        return std::nullopt;
    if (impl_->warmup_completed < impl_->options.policy.warmup) {
        return static_cast<std::uint64_t>(impl_->warmup_completed++);
    }
    if (impl_->measured >= impl_->ceiling())
        return std::nullopt;
    return static_cast<std::uint64_t>(impl_->measured++);
}

bool RepetitionScheduler::in_warmup() const noexcept {
    return impl_ && impl_->warmup_completed < impl_->options.policy.warmup;
}

std::uint32_t RepetitionScheduler::warmup_completed() const noexcept {
    return impl_ ? impl_->warmup_completed : 0;
}

std::uint32_t RepetitionScheduler::measured_count() const noexcept {
    return impl_ ? impl_->measured : 0;
}

bool RepetitionScheduler::observe(std::uint64_t elapsed_ns, const std::vector<double> &values) {
    if (!impl_)
        return false;
    impl_->elapsed_total_ns += elapsed_ns;
    for (double value : values) {
        Observation observation;
        observation.metric = "primary";
        observation.value = value;
        observation.valid = true;
        observation.validity = Validity::Valid;
        observation.sequence = impl_->observations.size();
        observation.repetition = impl_->measured;
        impl_->observations.push_back(observation);
    }
    const Estimates estimates = estimate(impl_->observations);
    const bool stop = should_stop(estimates);
    if (stop)
        impl_->stop_requested = true;
    return stop;
}

bool RepetitionScheduler::should_stop(const Estimates &estimates) const {
    if (!impl_ || !impl_->options.policy.adaptive)
        return false;
    if (impl_->measured < impl_->options.policy.repetitions)
        return false;
    if (impl_->options.policy.min_duration_ns != 0 &&
        impl_->elapsed_total_ns < impl_->options.policy.min_duration_ns)
        return false;
    if (impl_->options.policy.max_duration_ns != 0 &&
        impl_->elapsed_total_ns >= impl_->options.policy.max_duration_ns)
        return true;
    if (impl_->measured >= impl_->ceiling())
        return true;
    return estimates.valid >= 2 &&
           estimates.relative_standard_deviation <= impl_->options.policy.target_relative_stddev;
}

std::vector<std::size_t> RepetitionScheduler::interleave(std::size_t count) const {
    std::vector<std::size_t> indices(count);
    for (std::size_t index = 0; index < count; ++index)
        indices[index] = index;
    if (!impl_)
        return indices;
    // A copy of the RNG keeps interleave() deterministic per call site without
    // consuming the schedule's own random stream.
    Rng rng(impl_->options.seed != 0 ? impl_->options.seed : impl_->options.policy.seed);
    rng.shuffle(indices);
    return indices;
}

} // namespace v1
} // namespace moonstone::mahak
