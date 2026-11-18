#pragma once

#include "ParserGenerator.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace firestone::lr {

class LRParser {
public:
    explicit LRParser(const Grammar& grammar, LRMode mode = LRMode::lalr1)
        : LRParser(generateLRTable(grammar, mode)) {}

    explicit LRParser(LRTable table) : table_(std::move(table)) {
        if (!table_.conflicts().empty()) {
            throw std::invalid_argument("LRParser requires a conflict-free LR table");
        }
    }

    const LRTable& table() const { return table_; }

    // Returns no tree on syntax error. Public input omits the EOF token.
    std::optional<ParseNode> parse(const std::vector<Symbol>& input) const {
        const auto& grammar = table_.grammar();
        validateInput(grammar, input);
        std::vector<LRState> states{0};
        std::vector<ParseNode> nodes;
        std::size_t position = 0;
        for (;;) {
            const auto token = position == input.size() ? grammar.eof() : input[position];
            const auto& actions = table_.actions(states.back(), token);
            if (actions.empty()) return std::nullopt;
            const auto action = actions.front();
            if (action.kind == LRActionKind::accept) return std::move(nodes.back());
            if (action.kind == LRActionKind::shift) {
                states.push_back(action.target);
                nodes.push_back({token, position, position + 1, {}});
                ++position;
                continue;
            }

            const auto& production = grammar.productions()[action.target];
            const auto count = production.rhs.size();
            const auto first = nodes.size() - count;
            ParseNode parent{production.lhs, count == 0 ? position : nodes[first].begin, position, {}};
            parent.children.reserve(count);
            for (auto i = first; i < nodes.size(); ++i) parent.children.push_back(std::move(nodes[i]));
            nodes.resize(first);
            states.resize(states.size() - count);
            states.push_back(table_.goTo(states.back(), production.lhs).value());
            nodes.push_back(std::move(parent));
        }
    }

private:
    LRTable table_;
};

} // namespace firestone::lr
