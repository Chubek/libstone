#pragma once

#include <cstdint>

// Mahak is the Moonstone benchmarking suite. The inline namespace is the
// source-compatibility boundary: a future incompatible API must use v2 rather
// than silently changing v1 types. The C ABI exposes the same version.
namespace moonstone::mahak {
inline namespace v1 {

inline constexpr std::uint32_t mahak_api_version = 1;
inline constexpr std::uint32_t mahak_manifest_schema_version = 1;
inline constexpr std::uint32_t mahak_result_schema_version = 1;

inline constexpr const char *mahak_version_string = "0.1.0";

// Feature probes are compile-time facts about this build, not runtime claims.
inline constexpr bool has_linux_perf =
#if defined(__linux__)
    true;
#else
    false;
#endif
inline constexpr bool has_posix_spawn =
#if defined(__unix__) || defined(__APPLE__)
    true;
#else
    false;
#endif

} // namespace v1
} // namespace moonstone::mahak
