#include "mahak/Rng.hpp"

namespace moonstone::mahak {
inline namespace v1 {

struct Rng::State {
    std::uint64_t s[4] = {0, 0, 0, 0};
};

namespace {

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

std::uint64_t rotl(std::uint64_t value, int shift) noexcept {
    return (value << shift) | (value >> (64 - shift));
}

} // namespace

Rng::Rng(std::uint64_t seed) : state_(std::make_unique<State>()) {
    // A zero seed is a valid, deterministic stream. SplitMix64 expands it.
    std::uint64_t source = seed;
    for (auto &word : state_->s)
        word = splitmix64(source);
    if ((state_->s[0] | state_->s[1] | state_->s[2] | state_->s[3]) == 0)
        state_->s[0] = 1;
}

Rng::~Rng() = default;

Rng::Rng(Rng &&) noexcept = default;
Rng &Rng::operator=(Rng &&) noexcept = default;

std::uint64_t Rng::next_u64() noexcept {
    const std::uint64_t result = rotl(state_->s[1] * 5, 7) * 9;
    const std::uint64_t temporary = state_->s[1] << 17;
    state_->s[2] ^= state_->s[0];
    state_->s[3] ^= state_->s[1];
    state_->s[1] ^= state_->s[2];
    state_->s[0] ^= state_->s[3];
    state_->s[2] ^= temporary;
    state_->s[3] = rotl(state_->s[3], 45);
    return result;
}

double Rng::next_double() noexcept {
    // 53 bits is the mantissa width of IEEE double; the result is in [0,1).
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::size_t Rng::below(std::size_t bound) noexcept {
    if (bound == 0)
        return 0;
    const std::uint64_t limit = static_cast<std::uint64_t>(bound);
    const std::uint64_t threshold = (0 - limit) % limit;
    for (;;) {
        const std::uint64_t value = next_u64();
        if (value >= threshold)
            return static_cast<std::size_t>(value % limit);
    }
}

void Rng::shuffle(std::vector<std::size_t> &values) noexcept {
    for (std::size_t i = values.size(); i > 1; --i) {
        const std::size_t j = below(i);
        const std::size_t temporary = values[i - 1];
        values[i - 1] = values[j];
        values[j] = temporary;
    }
}

} // namespace v1
} // namespace moonstone::mahak
