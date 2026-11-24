#pragma once

#include "EBNF.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace firestone::lr {

using LRState = std::size_t;

enum class LRMode { canonicalLR1, lalr1, slr1 };
enum class LRActionKind { shift, reduce, accept };

struct LRAction {
    LRActionKind kind;
    std::size_t target; // State for shift, production ID for reduce, unused for accept.

    bool operator==(const LRAction& other) const { return kind == other.kind && target == other.target; }
};

struct LRConflict {
    LRState state;
    Symbol lookahead;
    std::vector<LRAction> actions;
};

class LRTable {
public:
    using ActionRow = std::map<Symbol, std::vector<LRAction>>;
    using GotoRow = std::map<Symbol, LRState>;

    const Grammar& grammar() const { return grammar_; }
    LRMode mode() const { return mode_; }
    std::size_t stateCount() const { return actions_.size(); }
    const std::vector<LRConflict>& conflicts() const { return conflicts_; }
    const ActionRow& actionRow(LRState state) const { return actions_.at(state); }
    const GotoRow& gotoRow(LRState state) const { return gotos_.at(state); }

    // State zero is the initial state. An empty action cell means syntax error.
    const std::vector<LRAction>& actions(LRState state, Symbol lookahead) const {
        static const std::vector<LRAction> empty;
        const auto& row = actionRow(state);
        const auto it = row.find(lookahead);
        return it == row.end() ? empty : it->second;
    }

    std::optional<LRState> goTo(LRState state, Symbol nonterminal) const {
        const auto& row = gotoRow(state);
        const auto it = row.find(nonterminal);
        return it == row.end() ? std::nullopt : std::optional<LRState>(it->second);
    }

private:
    Grammar grammar_;
    LRMode mode_;
    std::vector<ActionRow> actions_;
    std::vector<GotoRow> gotos_;
    std::vector<LRConflict> conflicts_;

    LRTable(Grammar grammar, LRMode mode) : grammar_(std::move(grammar)), mode_(mode) {}
    friend LRTable generateLRTable(const Grammar&, LRMode);
};

namespace detail {

struct LRItem {
    std::size_t production, dot;
    Symbol lookahead;

    bool operator<(const LRItem& other) const {
        return std::tie(production, dot, lookahead) < std::tie(other.production, other.dot, other.lookahead);
    }
};

using LRItemSet = std::vector<LRItem>; // Sorted and unique.

struct LRItemState {
    LRItemSet items;
    std::map<Symbol, LRState> transitions;
};

class LRItemBuilder {
public:
    LRItemBuilder(const Grammar& grammar, bool slr)
        : grammar_(grammar), slr_(slr), byLhs_(grammar.symbols().size()),
          useful_(slr ? usefulProductions(grammar) : std::vector<bool>{}),
          first_(grammar, slr ? &useful_ : nullptr) {
        for (std::size_t i = 0; i < grammar_.productions().size(); ++i) {
            if (!slr_ || useful_[i]) byLhs_[grammar_.productions()[i].lhs].push_back(i);
        }
    }

    std::vector<std::set<Symbol>> follow() const { return first_.follow(grammar_, slr_ ? &useful_ : nullptr); }

    std::size_t augmented() const { return grammar_.productions().size(); }

    bool complete(const LRItem& item) const {
        return item.dot == (item.production == augmented() ? 1 : grammar_.productions()[item.production].rhs.size());
    }

    std::vector<LRItemState> build() const {
        if (slr_ && byLhs_[grammar_.start()].empty()) return {LRItemState{}};
        const auto initial = closure({{augmented(), 0, grammar_.eof()}});
        std::vector<LRItemState> states{{initial, {}}};
        std::map<LRItemSet, LRState> ids{{initial, 0}};
        for (LRState i = 0; i < states.size(); ++i) {
            std::map<Symbol, LRItemSet> kernels;
            for (const auto& item : states[i].items) {
                if (!complete(item)) kernels[afterDot(item)].push_back({item.production, item.dot + 1, item.lookahead});
            }
            for (const auto& entry : kernels) {
                auto next = closure(entry.second);
                const auto inserted = ids.emplace(next, states.size());
                const auto destination = inserted.first->second;
                if (inserted.second) states.push_back({std::move(next), {}});
                states[i].transitions.emplace(entry.first, destination);
            }
        }
        return states;
    }

    std::vector<LRItemState> merge(const std::vector<LRItemState>& canonical) const {
        using Core = std::vector<std::pair<std::size_t, std::size_t>>;
        std::map<Core, LRState> ids;
        std::vector<LRState> group;
        std::vector<std::set<LRItem>> items;
        std::vector<LRItemState> merged;
        for (const auto& state : canonical) {
            Core core;
            for (const auto& item : state.items) {
                if (item.production != augmented() && item.dot == 0) continue;
                const auto position = std::make_pair(item.production, item.dot);
                if (core.empty() || core.back() != position) core.push_back(position);
            }
            const auto inserted = ids.emplace(std::move(core), merged.size());
            if (inserted.second) {
                merged.emplace_back();
                items.emplace_back();
            }
            const auto id = inserted.first->second;
            group.push_back(id);
            items[id].insert(state.items.begin(), state.items.end());
        }
        for (LRState i = 0; i < canonical.size(); ++i) {
            auto& transitions = merged[group[i]].transitions;
            for (const auto& edge : canonical[i].transitions) transitions.emplace(edge.first, group[edge.second]);
        }
        for (LRState i = 0; i < merged.size(); ++i) {
            merged[i].items.assign(items[i].begin(), items[i].end());
        }
        return merged;
    }

private:
    const Grammar& grammar_;
    bool slr_;
    std::vector<std::vector<std::size_t>> byLhs_;
    std::vector<bool> useful_;
    GrammarFirst first_;

    static std::vector<bool> usefulProductions(const Grammar& grammar) {
        // Dead contexts can introduce non-progressing reductions into LR(0)
        // states. Remove them before SLR analysis without renumbering rules.
        const auto& rules = grammar.productions();
        std::vector<bool> productive(grammar.symbols().size());
        for (Symbol s = 0; s < productive.size(); ++s) productive[s] = grammar.symbols()[s].terminal;
        std::vector<bool> productiveRules(rules.size());
        std::vector<std::vector<std::size_t>> byLhs(productive.size());
        bool changed;
        do {
            changed = false;
            for (std::size_t p = 0; p < rules.size(); ++p) {
                if (productiveRules[p]) continue;
                const auto& rule = rules[p];
                if (std::all_of(rule.rhs.begin(), rule.rhs.end(), [&](Symbol s) { return productive[s]; })) {
                    productiveRules[p] = true;
                    byLhs[rule.lhs].push_back(p);
                    changed |= !productive[rule.lhs];
                    productive[rule.lhs] = true;
                }
            }
        } while (changed);

        std::vector<bool> reachable(byLhs.size());
        std::vector<bool> useful(rules.size());
        std::vector<Symbol> work{grammar.start()};
        reachable[grammar.start()] = true;
        for (std::size_t i = 0; i < work.size(); ++i) {
            for (const auto production : byLhs[work[i]]) {
                useful[production] = true;
                for (const auto s : rules[production].rhs) {
                    if (!reachable[s] && !byLhs[s].empty()) {
                        reachable[s] = true;
                        work.push_back(s);
                    }
                }
            }
        }
        return useful;
    }

    Symbol afterDot(const LRItem& item) const {
        return item.production == augmented() ? grammar_.start() : grammar_.productions()[item.production].rhs[item.dot];
    }

    std::set<Symbol> lookaheads(const LRItem& item) const {
        // In LR(0) closure a fixed placeholder replaces actual lookaheads.
        if (slr_ || item.production == augmented()) return {item.lookahead};
        const auto& rhs = grammar_.productions()[item.production].rhs;
        auto result = first_.sequence(rhs, item.dot + 1);
        if (result.nullable) result.terminals.insert(item.lookahead);
        return std::move(result.terminals);
    }

    LRItemSet closure(const LRItemSet& kernel) const {
        std::set<LRItem> items(kernel.begin(), kernel.end());
        std::vector<LRItem> work(items.begin(), items.end());
        for (std::size_t i = 0; i < work.size(); ++i) {
            const auto item = work[i];
            if (complete(item)) continue;
            const auto next = afterDot(item);
            if (byLhs_[next].empty()) continue;
            const auto look = lookaheads(item);
            for (const auto production : byLhs_[next]) {
                for (const auto token : look) {
                    const LRItem nextItem{production, 0, token};
                    if (items.insert(nextItem).second) work.push_back(nextItem);
                }
            }
        }
        return {items.begin(), items.end()};
    }
};

} // namespace detail

// Conflicting actions are preserved; no precedence or implicit conflict
// resolution is applied. LALR merges equal LR(0) kernels of canonical states.
// SLR uses LR(0) states and FOLLOW of the LHS, considering only productive
// productions reachable from the start through other productive productions.
inline LRTable generateLRTable(const Grammar& grammar, LRMode mode = LRMode::canonicalLR1) {
    if (mode != LRMode::canonicalLR1 && mode != LRMode::lalr1 && mode != LRMode::slr1)
        throw std::invalid_argument("unknown LR table mode");
    grammar.validate();
    const bool slr = mode == LRMode::slr1;
    const detail::LRItemBuilder builder(grammar, slr);
    auto states = builder.build();
    if (mode == LRMode::lalr1) states = builder.merge(states);
    const auto follow = slr ? builder.follow() : std::vector<std::set<Symbol>>{};
    LRTable table(grammar, mode);
    table.actions_.resize(states.size());
    table.gotos_.resize(states.size());
    for (LRState i = 0; i < states.size(); ++i) {
        auto& row = table.actions_[i];
        auto addAction = [&](Symbol token, LRAction action) {
            auto& cell = row[token];
            if (std::find(cell.begin(), cell.end(), action) == cell.end()) cell.push_back(action);
        };
        for (const auto& edge : states[i].transitions) {
            if (grammar.symbols()[edge.first].terminal) addAction(edge.first, {LRActionKind::shift, edge.second});
            else table.gotos_[i].emplace(edge.first, edge.second);
        }
        for (const auto& item : states[i].items) {
            if (!builder.complete(item)) continue;
            if (item.production == builder.augmented()) {
                addAction(grammar.eof(), {LRActionKind::accept, 0});
            } else if (slr) {
                for (const auto token : follow[grammar.productions()[item.production].lhs])
                    addAction(token, {LRActionKind::reduce, item.production});
            } else {
                addAction(item.lookahead, {LRActionKind::reduce, item.production});
            }
        }
        for (const auto& entry : row) {
            if (entry.second.size() > 1) table.conflicts_.push_back({i, entry.first, entry.second});
        }
    }
    return table;
}

} // namespace firestone::lr
