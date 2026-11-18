#pragma once

#include "EBNF.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace firestone::lr {

// Borrowed token cursor for hand-written parsers. The input must stay alive and
// unchanged. A checkpoint is a token index.
class RecursiveDescent {
public:
    explicit RecursiveDescent(const std::vector<Symbol>& tokens) : tokens_(tokens) {}
    RecursiveDescent(const std::vector<Symbol>&&) = delete;
    RecursiveDescent(const RecursiveDescent&) = delete;
    RecursiveDescent& operator=(const RecursiveDescent&) = delete;

    RecursiveDescent(const Grammar& grammar, const std::vector<Symbol>& tokens)
        : RecursiveDescent(tokens) {
        grammar.validate();
        validateInput(grammar, tokens);
    }
    RecursiveDescent(const Grammar&, const std::vector<Symbol>&&) = delete;

    std::size_t position() const noexcept { return position_; }
    bool atEnd() const noexcept { return position_ == tokens_.size(); }

    std::optional<Symbol> peek(std::size_t offset = 0) const noexcept {
        if (offset >= tokens_.size() - position_) return std::nullopt;
        return tokens_[position_ + offset];
    }

    std::optional<Symbol> take() noexcept {
        if (atEnd()) return std::nullopt;
        return tokens_[position_++];
    }

    bool consume(Symbol symbol) noexcept {
        if (peek() != symbol) return false;
        ++position_;
        return true;
    }

    void restore(std::size_t checkpoint) {
        if (checkpoint > tokens_.size())
            throw std::out_of_range("recursive descent checkpoint outside input");
        position_ = checkpoint;
    }

    // A failed or throwing attempt restores the cursor, not outside side effects.
    template<class Function>
    bool attempt(Function&& parse) {
        const auto checkpoint = position_;
        try {
            if (std::invoke(std::forward<Function>(parse), *this)) {
                if (position_ < checkpoint)
                    throw std::logic_error("recursive descent attempt moved backward");
                return true;
            }
        } catch (...) {
            position_ = checkpoint;
            throw;
        }
        position_ = checkpoint;
        return false;
    }

    // Use a distinct id for each user rule. Calls such as A -> B -> A at the
    // same offset are diagnosed; right recursion after consuming input is legal.
    template<class Function>
    bool rule(Symbol id, Function&& parse) {
        const auto entry = active_.emplace(id, position_);
        if (!entry.second)
            throw std::invalid_argument("recursive descent left recursion in rule " +
                std::to_string(id) + " at token " + std::to_string(position_));
        const ActiveRule guard{active_, entry.first};
        return attempt(std::forward<Function>(parse));
    }

private:
    using Calls = std::set<std::pair<Symbol, std::size_t>>;
    struct ActiveRule {
        Calls& calls;
        Calls::iterator entry;
        ~ActiveRule() { calls.erase(entry); }
    };

    const std::vector<Symbol>& tokens_;
    std::size_t position_ = 0;
    Calls active_;
};

} // namespace firestone::lr
