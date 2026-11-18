#pragma once

#include "EBNF.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace firestone::lr {

template<class T>
struct CombinatorMatch {
    T value;
    std::size_t end;
};

// Value-producing, immutable-position parser. Failure leaves the starting
// offset available to alternatives. User callables should be free of side
// effects; their effects are not rolled back during backtracking or lookahead.
template<class T>
class ParserCombinator {
public:
    using Value = T;
    using Result = std::optional<CombinatorMatch<T>>;
    using Function = std::function<Result(const std::vector<Symbol>&, std::size_t)>;

    explicit ParserCombinator(Function function) : function_(std::move(function)) {
        if (!function_) throw std::invalid_argument("missing parser combinator function");
    }

    Result match(const std::vector<Symbol>& tokens, std::size_t begin = 0) const {
        if (begin > tokens.size())
            throw std::out_of_range("parser combinator position outside input");
        auto result = function_(tokens, begin);
        if (result && (result->end < begin || result->end > tokens.size()))
            throw std::logic_error("parser combinator returned invalid end position");
        return result;
    }

    std::optional<T> parse(const std::vector<Symbol>& tokens) const {
        auto result = match(tokens);
        if (!result || result->end != tokens.size()) return std::nullopt;
        return std::move(result->value);
    }

    std::optional<T> parse(const Grammar& grammar, const std::vector<Symbol>& tokens) const {
        grammar.validate();
        validateInput(grammar, tokens);
        return parse(tokens);
    }

    template<class Function>
    auto map(Function transform) const
        -> ParserCombinator<std::decay_t<std::invoke_result_t<const Function&, T>>> {
        using U = std::decay_t<std::invoke_result_t<const Function&, T>>;
        return ParserCombinator<U>([parser = *this, transform = std::move(transform)]
            (const std::vector<Symbol>& tokens, std::size_t begin)
            -> std::optional<CombinatorMatch<U>> {
                auto result = parser.match(tokens, begin);
                if (!result) return std::nullopt;
                return CombinatorMatch<U>{
                    std::invoke(transform, std::move(result->value)), result->end};
            });
    }

    template<class U>
    ParserCombinator<std::pair<T, U>> then(ParserCombinator<U> next) const {
        return ParserCombinator<std::pair<T, U>>([left = *this, right = std::move(next)]
            (const std::vector<Symbol>& tokens, std::size_t begin)
            -> std::optional<CombinatorMatch<std::pair<T, U>>> {
                auto a = left.match(tokens, begin);
                if (!a) return std::nullopt;
                auto b = right.match(tokens, a->end);
                if (!b) return std::nullopt;
                return CombinatorMatch<std::pair<T, U>>{
                    {std::move(a->value), std::move(b->value)}, b->end};
            });
    }

    ParserCombinator orElse(ParserCombinator alternative) const {
        return ParserCombinator([first = *this, second = std::move(alternative)]
            (const std::vector<Symbol>& tokens, std::size_t begin) -> Result {
                auto result = first.match(tokens, begin);
                if (result) return result;
                return second.match(tokens, begin);
            });
    }

    ParserCombinator<std::optional<T>> optional() const {
        return ParserCombinator<std::optional<T>>([parser = *this]
            (const std::vector<Symbol>& tokens, std::size_t begin) {
                auto result = parser.match(tokens, begin);
                if (!result) return CombinatorMatch<std::optional<T>>{std::nullopt, begin};
                return CombinatorMatch<std::optional<T>>{std::move(result->value), result->end};
            });
    }

    ParserCombinator<std::vector<T>> many(std::size_t minimum = 0) const {
        return ParserCombinator<std::vector<T>>([parser = *this, minimum]
            (const std::vector<Symbol>& tokens, std::size_t pos)
            -> std::optional<CombinatorMatch<std::vector<T>>> {
                std::vector<T> values;
                while (auto result = parser.match(tokens, pos)) {
                    if (result->end == pos)
                        throw std::invalid_argument("parser combinator repetition did not consume input");
                    pos = result->end;
                    values.push_back(std::move(result->value));
                }
                if (values.size() < minimum) return std::nullopt;
                return CombinatorMatch<std::vector<T>>{std::move(values), pos};
            });
    }

    ParserCombinator lookahead() const {
        return ParserCombinator([parser = *this]
            (const std::vector<Symbol>& tokens, std::size_t begin) {
                auto result = parser.match(tokens, begin);
                if (result) result->end = begin;
                return result;
            });
    }

    ParserCombinator<std::monostate> notFollowedBy() const {
        return ParserCombinator<std::monostate>([parser = *this]
            (const std::vector<Symbol>& tokens, std::size_t begin)
            -> std::optional<CombinatorMatch<std::monostate>> {
                if (parser.match(tokens, begin)) return std::nullopt;
                return CombinatorMatch<std::monostate>{{}, begin};
            });
    }

private:
    Function function_;
};

namespace combinator {

template<class Predicate>
ParserCombinator<Symbol> satisfy(Predicate predicate) {
    return ParserCombinator<Symbol>([predicate = std::move(predicate)]
        (const std::vector<Symbol>& tokens, std::size_t begin)
        -> std::optional<CombinatorMatch<Symbol>> {
            if (begin == tokens.size() || !std::invoke(predicate, tokens[begin])) return std::nullopt;
            return CombinatorMatch<Symbol>{tokens[begin], begin + 1};
        });
}

inline ParserCombinator<Symbol> token(Symbol expected) {
    return satisfy([expected](Symbol symbol) { return symbol == expected; });
}

template<class T>
ParserCombinator<T> pure(T value) {
    return ParserCombinator<T>([value = std::move(value)]
        (const std::vector<Symbol>&, std::size_t begin) {
            return CombinatorMatch<T>{value, begin};
        });
}

inline ParserCombinator<std::monostate> end() {
    return satisfy([](Symbol) { return true; }).notFollowedBy();
}

} // namespace combinator
} // namespace firestone::lr
