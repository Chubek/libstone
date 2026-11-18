#pragma once

#include "EBNF.hpp"
#include "RecursiveDescent.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace firestone::lr {

struct LLConflict {
    Symbol nonterminal;
    Symbol lookahead;
    // Indices into Grammar::productions(); all competing productions.
    std::vector<std::size_t> productions;
};

class LLTable {
public:
    explicit LLTable(Grammar grammar) : grammar_(std::move(grammar)) {
        grammar_.validate();
        const auto count = grammar_.symbols().size();
        nullable_.resize(count);
        first_.resize(count);
        follow_.resize(count);
        rows_.resize(count);
        for (Symbol symbol = 0; symbol < count; ++symbol)
            if (grammar_.symbols()[symbol].terminal) first_[symbol].insert(symbol);

        bool changed;
        do {
            changed = false;
            for (const auto& production : grammar_.productions()) {
                auto& lhs = first_[production.lhs];
                bool nullable = true;
                for (const auto symbol : production.rhs) {
                    changed = merge(lhs, first_[symbol]) || changed;
                    if (!nullable_[symbol]) {
                        nullable = false;
                        break;
                    }
                }
                if (nullable && !nullable_[production.lhs]) {
                    nullable_[production.lhs] = true;
                    changed = true;
                }
            }
        } while (changed);

        follow_[grammar_.start()].insert(grammar_.eof());
        do {
            changed = false;
            for (const auto& production : grammar_.productions()) {
                auto suffix = follow_[production.lhs];
                for (auto it = production.rhs.rbegin(); it != production.rhs.rend(); ++it) {
                    const auto symbol = *it;
                    if (!grammar_.symbols()[symbol].terminal)
                        changed = merge(follow_[symbol], suffix) || changed;
                    if (nullable_[symbol]) merge(suffix, first_[symbol]);
                    else suffix = first_[symbol];
                }
            }
        } while (changed);

        findLeftRecursion();
        std::vector<std::map<Symbol, std::vector<std::size_t>>> candidates(count);
        for (std::size_t i = 0; i < grammar_.productions().size(); ++i) {
            const auto& production = grammar_.productions()[i];
            std::set<Symbol> predict;
            bool nullable = true;
            for (const auto symbol : production.rhs) {
                merge(predict, first_[symbol]);
                if (!nullable_[symbol]) {
                    nullable = false;
                    break;
                }
            }
            if (nullable) merge(predict, follow_[production.lhs]);
            for (const auto token : predict) candidates[production.lhs][token].push_back(i);
        }
        for (Symbol lhs = 0; lhs < count; ++lhs) {
            for (auto& cell : candidates[lhs]) {
                if (cell.second.size() == 1) rows_[lhs].emplace(cell.first, cell.second.front());
                else conflicts_.push_back({lhs, cell.first, std::move(cell.second)});
            }
        }
    }

    const Grammar& grammar() const noexcept { return grammar_; }
    bool nullable(Symbol symbol) const { return nullable_.at(symbol); }
    const std::set<Symbol>& first(Symbol symbol) const { return first_.at(symbol); }
    const std::set<Symbol>& follow(Symbol symbol) const { return follow_.at(symbol); }
    const std::vector<LLConflict>& conflicts() const noexcept { return conflicts_; }
    // One closed left-corner cycle (including paths through nullable prefixes), or empty.
    const std::vector<Symbol>& leftRecursion() const noexcept { return leftRecursion_; }
    bool isLL1() const noexcept { return conflicts_.empty() && leftRecursion_.empty(); }

    // A conflicting or absent cell has no unique production.
    const std::map<Symbol, std::size_t>& row(Symbol lhs) const { return rows_.at(lhs); }
    std::optional<std::size_t> lookup(Symbol lhs, Symbol lookahead) const {
        const auto& entries = row(lhs);
        const auto it = entries.find(lookahead);
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }

    void requireLL1() const {
        if (!leftRecursion_.empty()) {
            std::string error = "LL(1) does not support left recursion: ";
            for (std::size_t i = 0; i < leftRecursion_.size(); ++i) {
                if (i != 0) error += " -> ";
                error += grammar_.symbols()[leftRecursion_[i]].name;
            }
            throw std::invalid_argument(error);
        }
        if (!conflicts_.empty()) {
            const auto& conflict = conflicts_.front();
            const auto lookahead = conflict.lookahead == grammar_.eof()
                ? std::string("EOF") : grammar_.symbols()[conflict.lookahead].name;
            std::string error = "LL(1) conflict for " + grammar_.symbols()[conflict.nonterminal].name
                + " on " + lookahead + ": productions";
            for (const auto index : conflict.productions) error += " " + std::to_string(index);
            throw std::invalid_argument(error);
        }
    }

private:
    static bool merge(std::set<Symbol>& into, const std::set<Symbol>& from) {
        if (&into == &from) return false;
        const auto before = into.size();
        into.insert(from.begin(), from.end());
        return before != into.size();
    }

    void findLeftRecursion() {
        std::vector<std::vector<Symbol>> edges(grammar_.symbols().size());
        for (const auto& production : grammar_.productions()) {
            for (const auto symbol : production.rhs) {
                if (grammar_.symbols()[symbol].terminal) break;
                edges[production.lhs].push_back(symbol);
                if (!nullable_[symbol]) break;
            }
        }

        // Iterative depth-first traversal: only a back edge to an active
        // ancestor represents a cycle.
        std::vector<unsigned char> color(edges.size());
        std::vector<std::pair<Symbol, std::size_t>> path;
        for (Symbol root = 0; root < edges.size(); ++root) {
            if (color[root] != 0) continue;
            path.emplace_back(root, 0);
            color[root] = 1;
            while (!path.empty()) {
                auto& frame = path.back();
                if (frame.second == edges[frame.first].size()) {
                    color[frame.first] = 2;
                    path.pop_back();
                    continue;
                }
                const auto child = edges[frame.first][frame.second++];
                if (color[child] == 1) {
                    const auto begin = std::find_if(path.begin(), path.end(),
                        [child](const auto& entry) { return entry.first == child; });
                    for (auto it = begin; it != path.end(); ++it) leftRecursion_.push_back(it->first);
                    leftRecursion_.push_back(child);
                    return;
                }
                if (color[child] == 0) {
                    color[child] = 1;
                    path.emplace_back(child, 0);
                }
            }
        }
    }

    Grammar grammar_;
    std::vector<bool> nullable_;
    std::vector<std::set<Symbol>> first_;
    std::vector<std::set<Symbol>> follow_;
    std::vector<std::map<Symbol, std::size_t>> rows_;
    std::vector<LLConflict> conflicts_;
    std::vector<Symbol> leftRecursion_;
};

class LLTableDriven {
public:
    explicit LLTableDriven(Grammar grammar) : LLTableDriven(LLTable(std::move(grammar))) {}
    explicit LLTableDriven(LLTable table) : table_(std::move(table)) { table_.requireLL1(); }

    const LLTable& table() const noexcept { return table_; }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        const auto& grammar = table_.grammar();
        validateInput(grammar, tokens);
        RecursiveDescent cursor(tokens);
        ParseNode root{grammar.start(), 0, 0, {}};
        struct Frame { ParseNode* node; bool closing; };
        std::vector<Frame> work{{&root, false}};

        while (!work.empty()) {
            const auto frame = work.back();
            work.pop_back();
            auto& node = *frame.node;
            if (frame.closing) {
                node.end = cursor.position();
                continue;
            }
            node.begin = cursor.position();
            if (grammar.symbols()[node.symbol].terminal) {
                if (!cursor.consume(node.symbol)) return std::nullopt;
                node.end = cursor.position();
                continue;
            }
            const auto choice = table_.lookup(node.symbol, cursor.peek().value_or(grammar.eof()));
            if (!choice) return std::nullopt;
            const auto& rhs = grammar.productions()[*choice].rhs;
            node.children.resize(rhs.size());
            work.push_back({&node, true});
            for (std::size_t i = rhs.size(); i != 0; --i) {
                node.children[i - 1].symbol = rhs[i - 1];
                work.push_back({&node.children[i - 1], false});
            }
        }
        if (!cursor.atEnd()) return std::nullopt;
        return root;
    }

private:
    LLTable table_;
};

} // namespace firestone::lr
