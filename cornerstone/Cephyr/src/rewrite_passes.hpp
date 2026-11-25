#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace cephyr {

// Bit positions are internal. The configuration schema exposes names only.
enum class FactIRPass { add_zero, boolize };
inline constexpr std::array<std::string_view, 2> factir_pass_names{"add-zero", "boolize"};

enum class BlackstonePass { constant_fold, identities, coalesce_copies, remove_dead_constants };
inline constexpr std::array<std::string_view, 4> blackstone_pass_names{
    "constant-fold", "identities", "coalesce-copies", "remove-dead-constants"};

enum class SandSLABPass { zero_extend_imm32, sign_extend_imm32 };
inline constexpr std::array<std::string_view, 2> sandslab_pass_names{
    "zero-extend-imm32", "sign-extend-imm32"};

template<class Pass>
constexpr bool pass_enabled(std::uint64_t mask, Pass pass)
{
    return (mask & (std::uint64_t{1} << static_cast<unsigned>(pass))) != 0;
}

} // namespace cephyr
