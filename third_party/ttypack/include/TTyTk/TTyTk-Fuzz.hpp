#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ttytk::fuzz {

namespace detail {
inline auto next(std::uint64_t& state) noexcept -> std::uint64_t {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 32U;
}
}  // namespace detail

/** @brief Generate exactly length bytes from printable, control and malformed fragments. */
[[nodiscard]] inline auto random_sequence(const std::size_t length,
                                          std::uint64_t seed = 0x5454795041434bULL) -> std::string {
    constexpr std::array<std::string_view, 9> fragments{
        "\x1b[31m", "\x1b]0;x\x07", "\x1bPq\x1b\\", "\xC3\x9B", "\x9B" "0m",
        "\x1b[1 2m", "\x1b]2;cancel\x18", "\x1b_ignored\x1b\\", "\x1b["};
    std::string output;
    output.reserve(length);
    while (output.size() < length) {
        const auto choice = detail::next(seed) % (fragments.size() + 2);
        if (choice < fragments.size()) output += fragments[choice].substr(0, length - output.size());
        else if (choice == fragments.size()) {
            output.push_back(static_cast<char>(0x20U + detail::next(seed) % 0x5fU));
        } else {
            output.push_back(static_cast<char>(detail::next(seed) & 0xFFU));
        }
    }
    return output;
}

/** @brief Change one byte, or generate a single byte for empty input. */
[[nodiscard]] inline auto mutate(const std::string_view input,
                                 std::uint64_t seed = 0x5454795041434bULL) -> std::string {
    std::string output{input};
    if (output.empty()) return random_sequence(1, seed);
    const auto position = static_cast<std::size_t>(detail::next(seed) % output.size());
    const auto bit = static_cast<unsigned>(detail::next(seed) % 8U);
    output[position] = static_cast<char>(static_cast<unsigned char>(output[position]) ^ (1U << bit));
    return output;
}

/** @brief Exercise an automaton with deterministic sequences and varied feed sizes. */
inline void stress_test(Automaton& automaton, const std::size_t iterations,
                        std::uint64_t seed = 0x5454795041434bULL) {
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto length = static_cast<std::size_t>(detail::next(seed) % 96U);
        const auto sequence = random_sequence(length, detail::next(seed));
        const std::string_view bytes{sequence};
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto chunk = std::min<std::size_t>(bytes.size() - offset,
                                                     1U + detail::next(seed) % 12U);
            automaton.feed(bytes.substr(offset, chunk));
            offset += chunk;
        }
    }
}

}  // namespace ttytk::fuzz
