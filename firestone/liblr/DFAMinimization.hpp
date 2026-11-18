#pragma once
#include "DFA.hpp"
#include <array>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace firestone::lr {

// Hopcroft refinement preserving the entire Acceptance record. Unreachable
// states and states equivalent to the implicit rejecting sink are discarded.
inline DFA minimize(const DFA& dfa) {
    const auto& states = dfa.states();
    if (states.empty()) throw std::invalid_argument("DFA: no start state");
    struct Incoming { StateId state; unsigned char byte; };
    std::vector<std::vector<Incoming>> incoming(states.size());
    std::vector<bool> reached(states.size());
    std::vector<StateId> reachable{dfa.start()};
    reached[dfa.start()] = true;
    std::vector<StateId> live;
    std::vector<bool> productive(states.size());
    for (std::size_t i = 0; i < reachable.size(); ++i) {
        auto state = reachable[i];
        if (states[state].acceptance) {
            live.push_back(state);
            productive[state] = true;
        }
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            auto target = states[state].transitions[byte];
            if (target == no_state) continue;
            incoming[target].push_back({state, static_cast<unsigned char>(byte)});
            if (!reached[target]) {
                reached[target] = true;
                reachable.push_back(target);
            }
        }
    }
    for (std::size_t i = 0; i < live.size(); ++i) {
        for (auto edge : incoming[live[i]]) {
            if (!productive[edge.state]) {
                productive[edge.state] = true;
                live.push_back(edge.state);
            }
        }
    }
    if (!productive[dfa.start()]) return DFA();

    struct Block {
        std::vector<StateId> members;
        bool pending = false;
    };
    std::vector<Block> blocks;
    std::vector<StateId> block_of(states.size(), no_state);
    std::vector<std::size_t> position(states.size());
    std::map<std::optional<Acceptance>, StateId> groups;
    for (auto state : live) {
        auto entry = groups.emplace(states[state].acceptance, blocks.size());
        if (entry.second) blocks.emplace_back();
        auto id = entry.first->second;
        block_of[state] = id;
        position[state] = blocks[id].members.size();
        blocks[id].members.push_back(state);
    }

    std::deque<StateId> work;
    auto enqueue = [&](StateId block) {
        work.push_back(block);
        blocks[block].pending = true;
    };
    for (StateId id = 0; id < blocks.size(); ++id) enqueue(id);
    std::array<std::vector<StateId>, byte_count> predecessors;
    std::vector<std::vector<StateId>> selected(live.size());
    std::vector<StateId> touched;
    while (!work.empty()) {
        auto splitter = work.front();
        work.pop_front();
        blocks[splitter].pending = false;
        // Snapshot all preimages before splitting any blocks (including splitter).
        for (auto target : blocks[splitter].members)
            for (auto edge : incoming[target]) predecessors[edge.byte].push_back(edge.state);
        for (auto& before : predecessors) {
            for (auto state : before) {
                auto id = block_of[state];
                if (selected[id].empty()) touched.push_back(id);
                selected[id].push_back(state);
            }
            for (auto id : touched) {
                auto& cut = selected[id];
                if (cut.size() != blocks[id].members.size()) {
                    auto split = blocks.size();
                    blocks.emplace_back();
                    auto& old = blocks[id].members;
                    auto& added = blocks[split].members;
                    for (auto state : cut) {
                        old[position[state]] = old.back();
                        position[old.back()] = position[state];
                        old.pop_back();
                        block_of[state] = split;
                        position[state] = added.size();
                        added.push_back(state);
                    }
                    // The old block is already in the queue if pending. Otherwise
                    // one half suffices: the old union has already been scheduled.
                    enqueue(blocks[id].pending || added.size() <= old.size() ? split : id);
                }
                cut.clear();
            }
            touched.clear();
            before.clear();
        }
    }

    // Number the quotient by breadth-first traversal from the start.
    std::vector<StateId> order{block_of[dfa.start()]};
    std::vector<StateId> number(blocks.size(), no_state);
    number[order.front()] = 0;
    std::vector<DFAState> result;
    result.reserve(blocks.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& from = states[blocks[order[i]].members.front()];
        result.emplace_back();
        result.back().acceptance = from.acceptance;
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            auto target = from.transitions[byte];
            if (target == no_state || !productive[target]) continue;
            auto block = block_of[target];
            if (number[block] == no_state) {
                number[block] = order.size();
                order.push_back(block);
            }
            result.back().transitions[byte] = number[block];
        }
    }
    return DFA(std::move(result));
}

} // namespace firestone::lr
