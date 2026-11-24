#pragma once
#include "NFA.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace firestone::lr {

struct DFAState {
    std::array<StateId, byte_count> transitions;
    std::optional<Acceptance> acceptance;

    DFAState() { transitions.fill(no_state); }
};

struct DFAMatch {
    std::size_t length;
    Acceptance acceptance;

    friend bool operator==(const DFAMatch& a, const DFAMatch& b) {
        return a.length == b.length && a.acceptance == b.acceptance;
    }
};

struct DFAByteClasses {
    // Bytes in the same class go to exactly the same target from every
    // state, including missing transitions. Classes are numbered in order
    // of their lowest byte, which is also their representative.
    std::array<unsigned char, byte_count> index{};
    std::vector<unsigned char> representatives;
};

// Partial DFA: no_state denotes a missing transition. The default DFA rejects all input.
class DFA {
    std::vector<DFAState> states_{1};
    StateId start_ = 0;

public:
    DFA() = default;

    explicit DFA(std::vector<DFAState> states, StateId start = 0)
        : states_(std::move(states)), start_(start) {
        if (start_ >= states_.size()) throw std::invalid_argument("DFA: invalid start state");
        for (const auto& state : states_)
            for (auto target : state.transitions)
                if (target != no_state && target >= states_.size())
                    throw std::invalid_argument("DFA: invalid transition");
    }

    StateId start() const { return start_; }
    const std::vector<DFAState>& states() const { return states_; }

    DFAByteClasses byte_classes() const {
        // Refine one group per distinguishing target in each row. The
        // representative keeps its group, so the map is used only on a split.
        DFAByteClasses result;
        std::array<unsigned char, byte_count> representative{};
        std::size_t width = 1;
        for (const auto& state : states_) {
            std::map<std::pair<unsigned char, StateId>, unsigned char> splits;
            for (std::size_t byte = 0; byte < byte_count; ++byte) {
                const auto cls = result.index[byte];
                const auto target = state.transitions[byte];
                if (target == state.transitions[representative[cls]]) continue;
                const auto entry = splits.try_emplace({cls, target}, static_cast<unsigned char>(width));
                if (entry.second) representative[width++] = static_cast<unsigned char>(byte);
                result.index[byte] = entry.first->second;
            }
            if (width == byte_count) break;
        }

        std::array<unsigned char, byte_count> numbered{};
        result.representatives.reserve(width);
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            const auto cls = result.index[byte];
            if (representative[cls] == byte) {
                numbered[cls] = static_cast<unsigned char>(result.representatives.size());
                result.representatives.push_back(static_cast<unsigned char>(byte));
            }
            result.index[byte] = numbered[cls];
        }
        return result;
    }

    std::optional<DFAMatch> match_prefix(std::string_view input) const {
        std::optional<DFAMatch> best;
        auto state = start_;
        if (states_.at(state).acceptance) best = DFAMatch{0, *states_[state].acceptance};
        const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
        for (std::size_t i = 0; i < input.size(); ++i) {
            state = states_[state].transitions[bytes[i]];
            if (state == no_state) break;
            if (states_[state].acceptance) best = DFAMatch{i + 1, *states_[state].acceptance};
        }
        return best;
    }
};

// Subset construction with a budget for distinct reachable subsets, including
// the start. Throws length_error rather than returning a partial DFA.
inline DFA determinize_bounded(const NFA& nfa, std::size_t max_states) {
    const auto& source = nfa.states();
    if (nfa.start() >= source.size()) throw std::invalid_argument("NFA: no start state");
    std::vector<bool> seen(source.size());
    auto close = [&](const std::vector<StateId>& seeds) {
        std::vector<StateId> result;
        auto visit = [&](StateId id) {
            if (!seen[id]) {
                result.push_back(id);
                seen[id] = true;
            }
        };
        for (auto id : seeds) visit(id);
        for (std::size_t i = 0; i < result.size(); ++i)
            for (auto id : source[result[i]].epsilon) visit(id);
        for (auto id : result) seen[id] = false;
        std::sort(result.begin(), result.end());
        return result;
    };

    std::map<std::vector<StateId>, StateId> indices;
    std::vector<const std::vector<StateId>*> subsets; // Map keys remain stable on insertion.
    std::vector<DFAState> states;
    auto intern = [&](std::vector<StateId> subset) {
        auto found = indices.lower_bound(subset);
        if (found != indices.end() && found->first == subset) return found->second;
        if (states.size() >= max_states) throw std::length_error("DFA: determinization state budget exceeded");
        auto entry = indices.emplace_hint(found, std::move(subset), states.size());
        subsets.push_back(&entry->first);
        states.emplace_back();
        return entry->second;
    };
    intern(close({nfa.start()}));
    std::array<std::vector<StateId>, byte_count> targets;
    for (StateId id = 0; id < subsets.size(); ++id) {
        for (const auto& accept_state : *subsets[id]) {
            const auto& state = source[accept_state];
            if (state.acceptance && (!states[id].acceptance || *state.acceptance < *states[id].acceptance))
                states[id].acceptance = state.acceptance;
            for (const auto& edge : state.edges)
                for (std::size_t byte = 0; byte < byte_count; ++byte)
                    if (edge.bytes[byte]) targets[byte].push_back(edge.target);
        }
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            if (targets[byte].empty()) continue;
            auto target = intern(close(targets[byte]));
            states[id].transitions[byte] = target;
            targets[byte].clear();
        }
    }
    return DFA(std::move(states));
}

// The number of reachable subsets can be exponential. Use
// determinize_bounded when the input NFA is subject to a state budget.
inline DFA determinize(const NFA& nfa) {
    return determinize_bounded(nfa, no_state);
}

} // namespace firestone::lr
