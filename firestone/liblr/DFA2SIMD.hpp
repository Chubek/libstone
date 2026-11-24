#pragma once
#include "LexerGenerator.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#if (defined(__clang__) || defined(__GNUC__)) && \
    ((defined(__x86_64__) && defined(__SSE2__)) || (defined(__aarch64__) && defined(__ARM_NEON)))
#define FIRESTONE_LR_VECTOR_BYTES 1
#endif

namespace firestone::lr {

namespace detail {

#if defined(FIRESTONE_LR_VECTOR_BYTES)
// GCC/Clang native 128-bit vectors. No CPU dispatch: these instructions must be
// available for the configured compiler target. No target headers are needed.
using ByteVector = unsigned char __attribute__((vector_size(16)));

inline ByteVector broadcast_byte(unsigned char byte) {
    return ByteVector{byte, byte, byte, byte, byte, byte, byte, byte,
                      byte, byte, byte, byte, byte, byte, byte, byte};
}
#endif

struct LoopRange {
    unsigned char low;
    unsigned char high;
};

struct LoopPlan {
    std::array<LoopRange, 4> ranges{};
    std::size_t count = 0;
    bool inverted = false;

    // Choose a compact representation of the set of bytes that leave a DFA
    // state unchanged (or of its complement). Complex sets stay scalar.
    static LoopPlan make(const DFAState& state, StateId id) {
        auto build = [&](bool inverted) {
            LoopPlan plan;
            plan.inverted = inverted;
            std::size_t byte = 0;
            auto inside = [&](std::size_t b) { return (state.transitions[b] == id) != inverted; };
            while (byte < byte_count) {
                if (!inside(byte)) {
                    ++byte;
                    continue;
                }
                auto first = byte++;
                while (byte < byte_count && inside(byte)) ++byte;
                if (plan.count == plan.ranges.size()) {
                    ++plan.count;
                    break;
                }
                plan.ranges[plan.count++] = {static_cast<unsigned char>(first), static_cast<unsigned char>(byte - 1)};
            }
            return plan;
        };
        auto direct = build(false), inverted = build(true);
        return direct.count <= inverted.count ? direct : inverted;
    }

    bool usable() const { return (count != 0 || inverted) && count <= ranges.size(); }
    bool all_bytes() const { return count == 0 && inverted; }

#if defined(FIRESTONE_LR_VECTOR_BYTES)
    bool all_match(const char* input) const {
        ByteVector bytes;
        std::memcpy(&bytes, input, sizeof(bytes)); // Caller guarantees 16 available bytes.
        auto mask = broadcast_byte(0);
        for (std::size_t i = 0; i < count; ++i) {
            auto low = broadcast_byte(ranges[i].low), high = broadcast_byte(ranges[i].high);
            mask |= static_cast<ByteVector>((bytes >= low) & (bytes <= high));
        }
        std::uint64_t halves[2];
        std::memcpy(halves, &mask, sizeof(halves));
        auto wanted = inverted ? std::uint64_t{0} : std::numeric_limits<std::uint64_t>::max();
        return halves[0] == wanted && halves[1] == wanted;
    }
#endif
};

} // namespace detail

// Vectorize long DFA self-loops by classifying 16 consecutive bytes at once.
// Other transitions run serially. With SIMD disabled/unavailable, use the DFA
// matcher directly; match results (including epsilon) are identical.
class SIMDMatcher {
    DFA dfa_;
    std::vector<detail::LoopPlan> loops_;
    bool enabled_;

public:
    static constexpr bool native_supported() {
#if defined(FIRESTONE_LR_VECTOR_BYTES)
        return true;
#else
        return false;
#endif
    }

    explicit SIMDMatcher(DFA dfa, bool enable_vectors = true)
        : dfa_(std::move(dfa)), enabled_(enable_vectors && native_supported()) {
        if (enabled_) {
            loops_.reserve(dfa_.states().size());
            for (StateId id = 0; id < dfa_.states().size(); ++id)
                loops_.push_back(detail::LoopPlan::make(dfa_.states()[id], id));
        }
    }

    bool simd_enabled() const { return enabled_; }
    const DFA& dfa() const { return dfa_; }

    std::optional<DFAMatch> match_prefix(std::string_view input) const {
#if defined(FIRESTONE_LR_VECTOR_BYTES)
        if (enabled_) {
            std::optional<DFAMatch> best;
            const auto& states = dfa_.states();
            auto state = dfa_.start();
            if (states.at(state).acceptance) best = DFAMatch{0, *states[state].acceptance};
            const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
            std::size_t offset = 0, next_vector = 0;
            while (offset < input.size()) {
                const auto& loop = loops_[state];
                if (loop.all_bytes()) {
                    // No later input can leave this state. Either it accepts
                    // the entire suffix, or the previous match remains best.
                    if (states[state].acceptance) best = DFAMatch{input.size(), *states[state].acceptance};
                    return best;
                }
                if (offset >= next_vector && loop.usable() && input.size() - offset >= 16) {
                    if (loop.all_match(input.data() + offset)) {
                        offset += 16;
                        if (states[state].acceptance) best = DFAMatch{offset, *states[state].acceptance};
                        continue;
                    }
                    // Use scalar transitions in this window until the state changes.
                    next_vector = offset + 16;
                }
                auto target = states[state].transitions[bytes[offset++]];
                if (target == no_state) break;
                if (target != state) next_vector = offset;
                state = target;
                if (states[state].acceptance) best = DFAMatch{offset, *states[state].acceptance};
            }
            return best;
        }
#endif
        return dfa_.match_prefix(input);
    }
};

class SIMDLexer {
    SIMDMatcher matcher_;

public:
    explicit SIMDLexer(DFA dfa, bool enable_vectors = true) : matcher_(std::move(dfa), enable_vectors) {
        detail::require_nonnullable(matcher_.dfa());
    }

    explicit SIMDLexer(const Lexer& lexer, bool enable_vectors = true) : SIMDLexer(lexer.dfa(), enable_vectors) {}

    bool simd_enabled() const { return matcher_.simd_enabled(); }
    const DFA& dfa() const { return matcher_.dfa(); }

    LexToken next(std::string_view input, std::size_t& offset) const {
        return detail::lexer_next(input, offset, [&](std::string_view tail) { return matcher_.match_prefix(tail); });
    }
};

} // namespace firestone::lr

#undef FIRESTONE_LR_VECTOR_BYTES
