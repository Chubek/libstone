#pragma once

#include "ParserGenerator.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace firestone::lr {

class LRParser {
public:
    explicit LRParser(const Grammar& grammar, LRMode mode = LRMode::lalr1)
        : LRParser(generateLRTable(grammar, mode)) {}

    explicit LRParser(LRTable table) : table_(std::move(table)) {
        if (table_.stateCount() == 0) throw std::invalid_argument("LRParser requires an initial state");
        if (!table_.conflicts().empty()) {
            const auto& conflict = table_.conflicts().front();
            std::string error = "LRParser requires a conflict-free LR table: state "
                + std::to_string(conflict.state) + " on "
                + table_.grammar().symbols()[conflict.lookahead].name + ":";
            for (const auto& action : conflict.actions) {
                switch (action.kind) {
                case LRActionKind::shift: error += " shift to " + std::to_string(action.target); break;
                case LRActionKind::reduce: error += " reduce by " + std::to_string(action.target); break;
                case LRActionKind::accept: error += " accept"; break;
                }
            }
            throw std::invalid_argument(error);
        }
    }

    const LRTable& table() const { return table_; }

    // Returns no tree on syntax error. Public input omits the EOF token.
    std::optional<ParseNode> parse(const std::vector<Symbol>& input) const {
        std::vector<ParseNode> nodes;
        if (!run(input, &nodes)) return std::nullopt;
        return std::move(nodes.back());
    }

    // Recognize without allocating parse nodes.
    bool accepts(const std::vector<Symbol>& input) const { return run(input, nullptr); }

private:
    bool run(const std::vector<Symbol>& input, std::vector<ParseNode>* nodes) const {
        const auto& grammar = table_.grammar();
        validateInput(grammar, input);
        std::vector<LRState> states{0};
        std::size_t position = 0;
        for (;;) {
            const auto token = position == input.size() ? grammar.eof() : input[position];
            const auto& actions = table_.actions(states.back(), token);
            if (actions.empty()) return false;
            const auto action = actions.front();
            if (action.kind == LRActionKind::accept) return true;
            if (action.kind == LRActionKind::shift) {
                states.push_back(action.target);
                if (nodes) nodes->push_back({token, position, position + 1, {}});
                ++position;
                continue;
            }

            const auto& production = grammar.productions()[action.target];
            const auto count = production.rhs.size();
            if (nodes) {
                const auto first = nodes->size() - count;
                ParseNode parent{production.lhs, count == 0 ? position : (*nodes)[first].begin, position, {}};
                parent.children.reserve(count);
                for (auto i = first; i < nodes->size(); ++i) parent.children.push_back(std::move((*nodes)[i]));
                nodes->resize(first);
                nodes->push_back(std::move(parent));
            }
            states.resize(states.size() - count);
            states.push_back(table_.goTo(states.back(), production.lhs).value());
        }
    }

    LRTable table_;
};

} // namespace firestone::lr
