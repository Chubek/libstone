#pragma once
#include "DFA.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace firestone::lr {

namespace detail {

enum class UTF8State : unsigned char {
    boundary, tail1, tail2, tail3, after_e0, after_ed, after_f0, after_f4, invalid
};

// The four special lead bytes restrict the first continuation. These bounds
// exclude overlong encodings, surrogate code points and values over U+10FFFF.
constexpr UTF8State utf8_step(UTF8State state, unsigned char byte) noexcept {
    using S = UTF8State;
    if (state == S::boundary) {
        if (byte <= 0x7f) return S::boundary;
        if (byte >= 0xc2 && byte <= 0xdf) return S::tail1;
        if (byte >= 0xe0 && byte <= 0xef) {
            if (byte == 0xe0) return S::after_e0;
            if (byte == 0xed) return S::after_ed;
            return S::tail2;
        }
        if (byte >= 0xf0 && byte <= 0xf4) {
            if (byte == 0xf0) return S::after_f0;
            if (byte == 0xf4) return S::after_f4;
            return S::tail3;
        }
        return S::invalid;
    }
    if (byte < 0x80 || byte > 0xbf) return S::invalid;
    switch (state) {
        case S::tail1: return S::boundary;
        case S::tail2: return S::tail1;
        case S::tail3: return S::tail2;
        case S::after_e0: return byte >= 0xa0 ? S::tail1 : S::invalid;
        case S::after_ed: return byte <= 0x9f ? S::tail1 : S::invalid;
        case S::after_f0: return byte >= 0x90 ? S::tail2 : S::invalid;
        case S::after_f4: return byte <= 0x8f ? S::tail2 : S::invalid;
        case S::boundary:
        case S::invalid: return S::invalid;
    }
    return S::invalid;
}

// Scalar automata split the stream boundary into distinct start and accept
// states and have no transitions out of the accept state.
inline constexpr StateId utf8_scalar_end = static_cast<StateId>(UTF8State::invalid);

constexpr StateId utf8_scalar_transition(StateId from, unsigned char byte) noexcept {
    const auto next = utf8_step(static_cast<UTF8State>(from), byte);
    if (next == UTF8State::invalid) return no_state;
    return next == UTF8State::boundary ? utf8_scalar_end : static_cast<StateId>(next);
}

} // namespace detail

enum class UTF8Status { Complete, Incomplete, Invalid };

// Incremental decoder for a stream of Unicode scalar values. Complete means
// a scalar boundary (including an empty stream); at EOF it is the only valid
// status. Invalid is sticky until reset. No replacement characters are emitted.
class UTF8Automaton {
public:
    constexpr UTF8Status status() const noexcept {
        if (state_ == State::invalid) return UTF8Status::Invalid;
        return state_ == State::boundary ? UTF8Status::Complete : UTF8Status::Incomplete;
    }

    constexpr UTF8Status push(unsigned char byte) noexcept {
        const auto previous = state_;
        state_ = detail::utf8_step(previous, byte);
        if (state_ != State::invalid) {
            if (previous == State::boundary) {
                const auto mask = byte <= 0x7f ? 0x7fu : byte <= 0xdf ? 0x1fu : byte <= 0xef ? 0x0fu : 0x07u;
                scalar_ = byte & mask;
            } else {
                scalar_ = (scalar_ << 6) | (byte & 0x3fu);
            }
            seen_scalar_ = state_ == State::boundary;
        }
        return status();
    }

    // Available only after a byte that completed a scalar, not initially.
    constexpr std::optional<char32_t> code_point() const noexcept {
        if (state_ != State::boundary || !seen_scalar_) return std::nullopt;
        return scalar_;
    }

    constexpr void reset() noexcept { *this = UTF8Automaton{}; }

private:
    using State = detail::UTF8State;
    State state_ = State::boundary;
    char32_t scalar_ = 0;
    bool seen_scalar_ = false;
};

struct UTF8Scalar {
    char32_t code_point;
    std::size_t length;
};

// Decode exactly the first scalar. Empty, malformed or unfinished prefixes
// return nullopt; bytes beyond the first scalar are not inspected.
constexpr std::optional<UTF8Scalar> decode_utf8_prefix(std::string_view input) noexcept {
    UTF8Automaton decoder;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto status = decoder.push(static_cast<unsigned char>(input[i]));
        if (status == UTF8Status::Invalid) return std::nullopt;
        if (status == UTF8Status::Complete) return UTF8Scalar{*decoder.code_point(), i + 1};
    }
    return std::nullopt;
}

// Offset of the first byte that has no valid transition, or the start of an
// unfinished scalar at EOF. nullopt means the entire input is valid UTF-8.
constexpr std::optional<std::size_t> utf8_error_offset(std::string_view input) noexcept {
    using S = detail::UTF8State;
    auto state = S::boundary;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (state == S::boundary) begin = i;
        state = detail::utf8_step(state, static_cast<unsigned char>(input[i]));
        if (state == S::invalid) return i;
    }
    if (state != S::boundary) return begin;
    return std::nullopt;
}

constexpr bool validate_utf8(std::string_view input) noexcept {
    return !utf8_error_offset(input);
}

// Byte DFA for exactly one scalar, usable with Lexer, minimization or
// code generation.
inline DFA utf8_scalar_dfa(Acceptance acceptance = {}) {
    constexpr auto end = detail::utf8_scalar_end;
    std::vector<DFAState> states(end + 1);
    states[end].acceptance = acceptance;
    for (StateId from = 0; from < end; ++from) {
        for (std::size_t byte = 0; byte < byte_count; ++byte)
            states[from].transitions[byte] = detail::utf8_scalar_transition(from, static_cast<unsigned char>(byte));
    }
    return DFA(std::move(states));
}

// NFA for exactly one scalar: can be composed with byte regex rules before
// determinization to provide a Unicode-aware lexer fallback.
inline NFA utf8_scalar_nfa(Acceptance acceptance = {}) {
    constexpr auto end = detail::utf8_scalar_end;
    NFA result;
    for (StateId id = 1; id <= end; ++id) result.add_state();
    for (StateId from = 0; from < end; ++from) {
        std::array<ByteSet, end + 1> by_target{};
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            const auto target = detail::utf8_scalar_transition(from, static_cast<unsigned char>(byte));
            if (target != no_state) by_target[target].set(byte);
        }
        for (StateId target = 0; target <= end; ++target)
            result.add_transition(from, by_target[target], target);
    }
    result.set_acceptance(end, acceptance);
    return result;
}

} // namespace firestone::lr
