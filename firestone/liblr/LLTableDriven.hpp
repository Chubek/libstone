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
#include <variant>
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
    explicit LLTable(Grammar grammar)
        : grammar_(std::move(grammar)), first_(analyze(grammar_)), follow_(first_.follow(grammar_)),
          rows_(grammar_.symbols().size()) {
        const auto count = grammar_.symbols().size();
        findLeftRecursion();
        std::vector<std::map<Symbol, std::vector<std::size_t>>> candidates(count);
        for (std::size_t i = 0; i < grammar_.productions().size(); ++i) {
            const auto& production = grammar_.productions()[i];
            auto predict = first_.sequence(production.rhs);
            if (predict.nullable) {
                const auto& follow = follow_[production.lhs];
                predict.terminals.insert(follow.begin(), follow.end());
            }
            for (const auto token : predict.terminals) candidates[production.lhs][token].push_back(i);
        }
        for (Symbol lhs = 0; lhs < count; ++lhs) {
            for (auto& cell : candidates[lhs]) {
                if (cell.second.size() == 1) rows_[lhs].emplace(cell.first, cell.second.front());
                else conflicts_.push_back({lhs, cell.first, std::move(cell.second)});
            }
        }
    }

    const Grammar& grammar() const noexcept { return grammar_; }
    bool nullable(Symbol symbol) const { return first_.nullable(symbol); }
    const std::set<Symbol>& first(Symbol symbol) const { return first_.first(symbol); }
    const std::set<Symbol>& follow(Symbol symbol) const { return follow_.at(symbol); }
    const std::vector<LLConflict>& conflicts() const noexcept { return conflicts_; }
    // One closed left-corner cycle (including paths through nullable prefixes), or empty.
    const std::vector<Symbol>& leftRecursion() const noexcept { return leftRecursion_; }
    bool isLL1() const noexcept { return !rows_.empty() && conflicts_.empty() && leftRecursion_.empty(); }

    // A conflicting or absent cell has no unique production.
    const std::map<Symbol, std::size_t>& row(Symbol lhs) const { return rows_.at(lhs); }
    std::optional<std::size_t> lookup(Symbol lhs, Symbol lookahead) const {
        const auto& entries = row(lhs);
        const auto it = entries.find(lookahead);
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }

    void requireLL1() const {
        if (rows_.empty()) throw std::invalid_argument("missing LL(1) table rows");
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
    static detail::GrammarFirst analyze(const Grammar& grammar) {
        grammar.validate();
        return detail::GrammarFirst(grammar);
    }

    void findLeftRecursion() {
        std::vector<std::vector<Symbol>> edges(grammar_.symbols().size());
        for (const auto& production : grammar_.productions()) {
            for (const auto symbol : production.rhs) {
                if (grammar_.symbols()[symbol].terminal) break;
                edges[production.lhs].push_back(symbol);
                if (!first_.nullable(symbol)) break;
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
    detail::GrammarFirst first_;
    std::vector<std::set<Symbol>> follow_;
    std::vector<std::map<Symbol, std::size_t>> rows_;
    std::vector<LLConflict> conflicts_;
    std::vector<Symbol> leftRecursion_;
};

struct LLSyntaxError {
    std::size_t position; // Token index; tokens.size() at end of input.
    Symbol found;         // Grammar::eof() when input is exhausted.
    std::vector<Symbol> expected;
};

class LLTableDriven {
public:
    explicit LLTableDriven(Grammar grammar) : LLTableDriven(LLTable(std::move(grammar))) {}
    explicit LLTableDriven(LLTable table) : table_(std::move(table)) { table_.requireLL1(); }

    const LLTable& table() const noexcept { return table_; }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        return parseImpl(tokens, nullptr);
    }

    // Reports lookaheads for the failing table row, or the literal terminal
    // (including EOF) required by the stack. Invalid input symbols still throw.
    std::variant<ParseNode, LLSyntaxError> parseWithDiagnostics(const std::vector<Symbol>& tokens) const {
        LLSyntaxError error{};
        if (auto root = parseImpl(tokens, &error)) return std::move(*root);
        return error;
    }

private:
    std::optional<ParseNode> parseImpl(const std::vector<Symbol>& tokens, LLSyntaxError* error) const {
        const auto& grammar = table_.grammar();
        validateInput(grammar, tokens);
        RecursiveDescent cursor(tokens);
        const auto lookahead = [&] { return cursor.peek().value_or(grammar.eof()); };
        const auto fail = [&](Symbol expected) {
            if (error) *error = {cursor.position(), lookahead(), {expected}};
            return std::nullopt;
        };
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
                if (!cursor.consume(node.symbol)) return fail(node.symbol);
                node.end = cursor.position();
                continue;
            }
            const auto token = lookahead();
            const auto choice = table_.lookup(node.symbol, token);
            if (!choice) {
                if (error) {
                    const auto& row = table_.row(node.symbol);
                    *error = {cursor.position(), token, {}};
                    error->expected.reserve(row.size());
                    for (const auto& entry : row) error->expected.push_back(entry.first);
                }
                return std::nullopt;
            }
            const auto& rhs = grammar.productions()[*choice].rhs;
            node.children.resize(rhs.size());
            work.push_back({&node, true});
            for (std::size_t i = rhs.size(); i != 0; --i) {
                node.children[i - 1].symbol = rhs[i - 1];
                work.push_back({&node.children[i - 1], false});
            }
        }
        if (!cursor.atEnd()) return fail(grammar.eof());
        return root;
    }

    LLTable table_;
};

} // namespace firestone::lr
