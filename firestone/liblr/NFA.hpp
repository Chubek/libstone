#pragma once
#include <bitset>
#include <climits>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
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

    // Copy a disjoint alternative and connect it by epsilon from this start.
    // Existing state IDs and both automata's acceptance records are preserved.
    // Self-composition is allowed.
    void add_alternative(const NFA& alternative) {
        const auto base = states_.size();
        if (start_ >= base || alternative.start_ >= alternative.states_.size())
            throw std::invalid_argument("NFA: no start state");
        if (alternative.states_.size() > states_.max_size() - base)
            throw std::length_error("NFA: too many states");
        auto added = alternative.states_;
        for (auto& state : added) {
            for (auto& target : state.epsilon) target += base;
            for (auto& edge : state.edges) edge.target += base;
        }
        states_[start_].epsilon.push_back(base + alternative.start_);
        try {
            states_.insert(states_.end(), std::make_move_iterator(added.begin()),
                           std::make_move_iterator(added.end()));
        } catch (...) {
            states_[start_].epsilon.pop_back();
            throw;
        }
    }
};

} // namespace firestone::lr
