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
    std::size_t remaining() const noexcept { return tokens_.size() - position_; }
    bool atEnd() const noexcept { return remaining() == 0; }

    std::optional<Symbol> peek(std::size_t offset = 0) const noexcept {
        if (offset >= remaining()) return std::nullopt;
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

    // Match the entire sequence or leave the cursor unchanged.
    bool consumeSequence(const std::vector<Symbol>& symbols) noexcept {
        if (symbols.size() > remaining()) return false;
        for (std::size_t i = 0; i < symbols.size(); ++i)
            if (tokens_[position_ + i] != symbols[i]) return false;
        position_ += symbols.size();
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
        return transaction(std::forward<Function>(parse), true);
    }

    // Probe a parser without committing any cursor movement.
    template<class Function>
    bool lookahead(Function&& parse) {
        return transaction(std::forward<Function>(parse), false);
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
    struct Checkpoint {
        std::size_t& position;
        std::size_t saved;
        bool commit = false;
        ~Checkpoint() { if (!commit) position = saved; }
    };

    template<class Function>
    bool transaction(Function&& parse, bool consume) {
        Checkpoint guard{position_, position_};
        if (!std::invoke(std::forward<Function>(parse), *this)) return false;
        if (position_ < guard.saved)
            throw std::logic_error("recursive descent attempt moved backward");
        guard.commit = consume;
        return true;
    }

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
