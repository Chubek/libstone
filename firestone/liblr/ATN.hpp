#pragma once

#include "GLL.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace firestone::lr {

// CFG-derived augmented transition network. Each nonterminal is a subnetwork
// with an entry, epsilon branches for alternatives, and a shared return state.
// A call arc names the target nonterminal; its target field is the continuation
// after return. No semantic predicates, registers, or actions are attached.
class ATN {
public:
    using StateId = std::size_t;
    using ArcKind = detail::TopDownArcKind;
    using Arc = detail::TopDownArc;

    explicit ATN(Grammar grammar) : grammar_(std::move(grammar)) {
        grammar_.validate();
        network_.entries.resize(grammar_.symbols().size());
        std::vector<StateId> exits(grammar_.symbols().size());
        for (Symbol rule = 0; rule < grammar_.symbols().size(); ++rule) {
            if (grammar_.symbols()[rule].terminal) continue;
            network_.entries[rule] = appendState();
            exits[rule] = appendState();
            network_.states[exits[rule]].push_back({ArcKind::return_, 0, 0});
        }
        for (const auto& production : grammar_.productions()) {
            const auto entryState = *network_.entries[production.lhs];
            const auto branch = production.rhs.empty() ? exits[production.lhs] : appendState();
            network_.states[entryState].push_back({ArcKind::epsilon, branch, 0});
            auto state = branch;
            for (std::size_t i = 0; i < production.rhs.size(); ++i) {
                const auto symbol = production.rhs[i];
                const auto next = i + 1 == production.rhs.size()
                    ? exits[production.lhs] : appendState();
                const auto kind = grammar_.symbols()[symbol].terminal ? ArcKind::match : ArcKind::call;
                network_.states[state].push_back({kind, next, symbol});
                state = next;
            }
        }
    }

    const Grammar& grammar() const noexcept { return grammar_; }
    const std::vector<std::vector<Arc>>& states() const noexcept { return network_.states; }
    std::optional<StateId> entry(Symbol symbol) const { return network_.entries.at(symbol); }

    bool accepts(const std::vector<Symbol>& tokens) const {
        return detail::topDownAccepts(network_, grammar_, tokens);
    }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        return detail::topDownParse(network_, grammar_, tokens);
    }

private:
    struct Network {
        std::vector<std::optional<StateId>> entries;
        std::vector<std::vector<Arc>> states;

        template<class Function>
        void enter(Symbol symbol, Function&& visit) const { visit(*entries[symbol]); }

        template<class Function>
        void step(StateId slot, Function&& visit) const {
            for (const auto& arc : states[slot]) visit(arc);
        }
    };

    StateId appendState() {
        const auto state = network_.states.size();
        network_.states.emplace_back();
        return state;
    }

    Grammar grammar_;
    Network network_;
};

} // namespace firestone::lr
