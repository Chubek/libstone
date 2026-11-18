#pragma once

#include "LLTableDriven.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace firestone::lr {

// Compiles each nonterminal into a callable with a lookahead decision tree.
// Leaves call other nonterminal functions directly through the rule vector.
class LLFunctionDriven {
public:
    explicit LLFunctionDriven(Grammar grammar) : LLFunctionDriven(LLTable(std::move(grammar))) {}
    explicit LLFunctionDriven(LLTable table) : grammar_(table.grammar()) {
        table.requireLL1();
        rules_.resize(grammar_.symbols().size());
        for (Symbol rule = 0; rule < rules_.size(); ++rule) {
            if (grammar_.symbols()[rule].terminal) continue;
            const auto& row = table.row(rule);
            const std::vector<Case> choices(row.begin(), row.end());
            rules_[rule] = compile(choices, 0, choices.size());
        }
    }

    const Grammar& grammar() const noexcept { return grammar_; }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        validateInput(grammar_, tokens);
        RecursiveDescent cursor(tokens);
        auto root = parseRule(grammar_.start(), cursor);
        if (!cursor.atEnd()) return std::nullopt;
        return root;
    }

private:
    using Case = std::pair<Symbol, std::size_t>;
    using Rule = std::function<bool(const LLFunctionDriven&, RecursiveDescent&, ParseNode&, Symbol)>;

    Rule compile(const std::vector<Case>& choices, std::size_t lo, std::size_t hi) const {
        if (lo == hi)
            return [](const LLFunctionDriven&, RecursiveDescent&, ParseNode&, Symbol) { return false; };
        const auto middle = lo + (hi - lo) / 2;
        const auto& choice = choices[middle];
        return [lookahead = choice.first, body = grammar_.productions()[choice.second].rhs,
                less = compile(choices, lo, middle), more = compile(choices, middle + 1, hi)]
               (const LLFunctionDriven& parser, RecursiveDescent& cursor, ParseNode& node, Symbol token) {
            if (token < lookahead) return less(parser, cursor, node, token);
            if (token > lookahead) return more(parser, cursor, node, token);
            node.children.reserve(body.size());
            for (const auto symbol : body) {
                if (parser.grammar_.symbols()[symbol].terminal) {
                    const auto begin = cursor.position();
                    if (!cursor.consume(symbol)) return false;
                    node.children.push_back({symbol, begin, cursor.position(), {}});
                } else {
                    auto child = parser.parseRule(symbol, cursor);
                    if (!child) return false;
                    node.children.push_back(std::move(*child));
                }
            }
            return true;
        };
    }

    std::optional<ParseNode> parseRule(Symbol symbol, RecursiveDescent& cursor) const {
        ParseNode node{symbol, cursor.position(), cursor.position(), {}};
        if (!rules_[symbol](*this, cursor, node, cursor.peek().value_or(grammar_.eof())))
            return std::nullopt;
        node.end = cursor.position();
        return node;
    }

    Grammar grammar_;
    std::vector<Rule> rules_;
};

} // namespace firestone::lr
