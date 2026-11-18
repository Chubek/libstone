#pragma once

#include "EBNF.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace firestone::lr {

using LRState = std::size_t;

enum class LRMode { canonicalLR1, lalr1 };
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
    const Grammar& grammar() const { return grammar_; }
    LRMode mode() const { return mode_; }
    std::size_t stateCount() const { return actions_.size(); }
    const std::vector<LRConflict>& conflicts() const { return conflicts_; }

    // State zero is the initial state. An empty action cell means syntax error.
    const std::vector<LRAction>& actions(LRState state, Symbol lookahead) const {
        static const std::vector<LRAction> empty;
        const auto& row = actions_.at(state);
        const auto it = row.find(lookahead);
        return it == row.end() ? empty : it->second;
    }

    std::optional<LRState> goTo(LRState state, Symbol nonterminal) const {
        const auto& row = gotos_.at(state);
        const auto it = row.find(nonterminal);
        return it == row.end() ? std::nullopt : std::optional<LRState>(it->second);
    }

private:
    Grammar grammar_;
    LRMode mode_;
    std::vector<std::map<Symbol, std::vector<LRAction>>> actions_;
    std::vector<std::map<Symbol, LRState>> gotos_;
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
    explicit LRItemBuilder(const Grammar& grammar)
        : grammar_(grammar), byLhs_(grammar.symbols().size()), first_(byLhs_.size()), nullable_(byLhs_.size()) {
        for (std::size_t i = 0; i < grammar_.productions().size(); ++i) {
            byLhs_[grammar_.productions()[i].lhs].push_back(i);
        }
        for (Symbol s = 0; s < first_.size(); ++s) {
            if (grammar_.symbols()[s].terminal) first_[s].insert(s);
        }
        bool changed;
        do {
            changed = false;
            for (const auto& rule : grammar_.productions()) {
                bool allNullable = true;
                for (const auto s : rule.rhs) {
                    changed |= unite(first_[rule.lhs], first_[s]);
                    if (!nullable_[s]) {
                        allNullable = false;
                        break;
                    }
                }
                if (allNullable && !nullable_[rule.lhs]) {
                    nullable_[rule.lhs] = true;
                    changed = true;
                }
            }
        } while (changed);
    }

    std::size_t augmented() const { return grammar_.productions().size(); }

    bool complete(const LRItem& item) const {
        return item.dot == (item.production == augmented() ? 1 : grammar_.productions()[item.production].rhs.size());
    }

    std::vector<LRItemState> canonical() const {
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
    std::vector<std::vector<std::size_t>> byLhs_;
    std::vector<std::set<Symbol>> first_;
    std::vector<bool> nullable_;

    static bool unite(std::set<Symbol>& into, const std::set<Symbol>& from) {
        if (&into == &from) return false;
        const auto before = into.size();
        into.insert(from.begin(), from.end());
        return into.size() != before;
    }

    Symbol afterDot(const LRItem& item) const {
        return item.production == augmented() ? grammar_.start() : grammar_.productions()[item.production].rhs[item.dot];
    }

    std::set<Symbol> lookaheads(const LRItem& item) const {
        std::set<Symbol> result;
        if (item.production != augmented()) {
            const auto& rhs = grammar_.productions()[item.production].rhs;
            for (auto i = item.dot + 1; i < rhs.size(); ++i) {
                unite(result, first_[rhs[i]]);
                if (!nullable_[rhs[i]]) return result;
            }
        }
        result.insert(item.lookahead);
        return result;
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
inline LRTable generateLRTable(const Grammar& grammar, LRMode mode = LRMode::canonicalLR1) {
    grammar.validate();
    const detail::LRItemBuilder builder(grammar);
    auto states = builder.canonical();
    if (mode == LRMode::lalr1) states = builder.merge(states);
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
            if (item.production == builder.augmented()) addAction(grammar.eof(), {LRActionKind::accept, 0});
            else addAction(item.lookahead, {LRActionKind::reduce, item.production});
        }
        for (const auto& entry : row) {
            if (entry.second.size() > 1) table.conflicts_.push_back({i, entry.first, entry.second});
        }
    }
    return table;
}

} // namespace firestone::lr
