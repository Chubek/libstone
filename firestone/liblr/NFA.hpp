#pragma once
#include <bitset>
#include <climits>
#include <cstddef>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

namespace firestone::lr {

static_assert(CHAR_BIT == 8, "Firestone liblr uses 8-bit bytes");
inline constexpr std::size_t byte_count = 256;
using ByteSet = std::bitset<byte_count>;
using StateId = std::size_t;
inline constexpr StateId no_state = std::numeric_limits<StateId>::max();

struct Acceptance {
    std::size_t token = 0;
    std::size_t priority = 0;
    bool skip = false;

    friend bool operator==(const Acceptance& a, const Acceptance& b) {
        return a.priority == b.priority && a.token == b.token && a.skip == b.skip;
    }

    // Lower priority wins. Hand-built NFAs break ties by token ID, then prefer non-skip.
    friend bool operator<(const Acceptance& a, const Acceptance& b) {
        return std::tie(a.priority, a.token, a.skip) < std::tie(b.priority, b.token, b.skip);
    }
};

struct NFAEdge {
    ByteSet bytes;
    StateId target;
};

struct NFAState {
    std::vector<StateId> epsilon;
    std::vector<NFAEdge> edges;
    std::optional<Acceptance> acceptance;
};

// An NFA starts with one state (0); all mutation checks state IDs.
class NFA {
    std::vector<NFAState> states_{1};
    StateId start_ = 0;

public:
    StateId add_state() {
        states_.emplace_back();
        return states_.size() - 1;
    }

    StateId start() const { return start_; }
    const std::vector<NFAState>& states() const { return states_; }

    void set_start(StateId id) {
        (void)states_.at(id);
        start_ = id;
    }

    void set_acceptance(StateId id, std::optional<Acceptance> acceptance) {
        states_.at(id).acceptance = acceptance;
    }

    void add_epsilon(StateId from, StateId to) {
        (void)states_.at(to);
        states_.at(from).epsilon.push_back(to);
    }

    void add_transition(StateId from, ByteSet bytes, StateId to) {
        (void)states_.at(to);
        auto& state = states_.at(from);
        if (bytes.any()) state.edges.push_back({bytes, to});
    }
};

} // namespace firestone::lr
