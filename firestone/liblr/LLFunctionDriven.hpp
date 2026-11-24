#pragma once

#include "LLTableDriven.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace firestone::lr {

// Compiles each nonterminal into a callable lookahead decision tree.
// An explicit stack drives the selected productions, including right recursion.
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

    bool accepts(const std::vector<Symbol>& tokens) const { return run(tokens, nullptr); }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        std::vector<Expansion> trace;
        if (!run(tokens, &trace)) return std::nullopt;
        // Replay the successful preorder trace backward. Rejected inputs never
        // construct a partial (possibly deeply nested) tree to tear down.
        std::vector<ParseNode> nodes;
        for (auto it = trace.rbegin(); it != trace.rend(); ++it) {
            const auto& production = grammar_.productions()[it->production];
            ParseNode node{production.lhs, it->begin, it->end, {}};
            node.children.resize(production.rhs.size());
            auto pos = it->begin;
            for (std::size_t i = 0; i < production.rhs.size(); ++i) {
                const auto symbol = production.rhs[i];
                auto& child = node.children[i];
                if (grammar_.symbols()[symbol].terminal) {
                    child = {symbol, pos, pos + 1, {}};
                } else {
                    child = std::move(nodes.back());
                    nodes.pop_back();
                }
                pos = child.end;
            }
            nodes.push_back(std::move(node));
        }
        return std::move(nodes.back());
    }

private:
    using Case = std::pair<Symbol, std::size_t>;
    using Rule = std::function<std::optional<std::size_t>(Symbol)>;
    struct Expansion { std::size_t production, begin, end; };
    struct Frame { std::size_t production, next, expansion; };

    static Rule compile(const std::vector<Case>& choices, std::size_t lo, std::size_t hi) {
        if (lo == hi)
            return [](Symbol) -> std::optional<std::size_t> { return std::nullopt; };
        const auto middle = lo + (hi - lo) / 2;
        const auto& choice = choices[middle];
        return [choice,
                less = compile(choices, lo, middle), more = compile(choices, middle + 1, hi)]
               (Symbol token) -> std::optional<std::size_t> {
            if (token < choice.first) return less(token);
            if (token > choice.first) return more(token);
            return choice.second;
        };
    }

    bool run(const std::vector<Symbol>& tokens, std::vector<Expansion>* trace) const {
        validateInput(grammar_, tokens);
        RecursiveDescent cursor(tokens);
        std::vector<Frame> stack;
        const auto enter = [&](Symbol symbol) {
            const auto production = rules_[symbol](cursor.peek().value_or(grammar_.eof()));
            if (!production) return false;
            stack.push_back({*production, 0, trace ? trace->size() : 0});
            if (trace) trace->push_back({*production, cursor.position(), 0});
            return true;
        };
        if (!enter(grammar_.start())) return false;
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto& rhs = grammar_.productions()[frame.production].rhs;
            if (frame.next == rhs.size()) {
                if (trace) (*trace)[frame.expansion].end = cursor.position();
                stack.pop_back();
                continue;
            }
            const auto symbol = rhs[frame.next++];
            if (grammar_.symbols()[symbol].terminal) {
                if (!cursor.consume(symbol)) return false;
            } else if (!enter(symbol)) {
                return false;
            }
        }
        return cursor.atEnd();
    }

    Grammar grammar_;
    std::vector<Rule> rules_;
};

} // namespace firestone::lr
