#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace moonstone::mahak {
inline namespace v1 {

// Deterministic SplitMix64-seeded xoshiro256** stream. The exact sequence is
// part of Mahak's reproducibility contract for a fixed 64-bit seed.
class Rng {
  public:
    explicit Rng(std::uint64_t seed = 0);
    ~Rng();
    Rng(Rng &&) noexcept;
    Rng &operator=(Rng &&) noexcept;
    Rng(const Rng &) = delete;
    Rng &operator=(const Rng &) = delete;

    std::uint64_t next_u64() noexcept;
    double next_double() noexcept; // [0,1)
    std::size_t below(std::size_t bound) noexcept;
    void shuffle(std::vector<std::size_t> &values) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace v1
} // namespace moonstone::mahak
