#pragma once

#include "EBNF.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace firestone::lr {

enum class LRDiagPhase { lexical, syntactic };
enum class LRDiagSeverity { patch, inform, warning, error };

// Borrowed views and pointers refer to the current input for the duration of
// diagnose(). Byte spans are half-open; use [0, 0) if no source span is available.
// In lexical state, tokens may contain the successfully scanned prefix.
// Syntactic state requires both the grammar and the current token stream.
struct LRDiagState {
    LRDiagPhase phase;
    std::string_view source;
    std::string_view provenance;
    std::size_t byteBegin, byteEnd;
    const Grammar* grammar = nullptr;
    const std::vector<Symbol>* tokens = nullptr;
    std::size_t tokenPosition = 0;
    std::optional<Symbol> found;
    std::vector<Symbol> expected;
};

// Edits refer to the current input, not to original pre-edit coordinates.
// Both ranges are half-open; an empty range denotes an insertion.
struct LRBytePatch {
    std::size_t begin, end;
    std::string replacement;
};

struct LRTokenPatch {
    std::size_t begin, end;
    std::vector<Symbol> replacement;
};

using LRDiagPatch = std::variant<LRBytePatch, LRTokenPatch>;

struct LRDiagAction {
    LRDiagSeverity severity;
    std::string message;
    std::optional<LRDiagPatch> patch;
    std::string suggestion;
};

class LRDiagEngine {
public:
    LRDiagEngine& addRule(std::function<bool(const LRDiagState&)> matches,
                          std::function<LRDiagAction(const LRDiagState&)> action) {
        if (!matches || !action)
            throw std::invalid_argument("diagnostic rule requires a predicate and an action");
        rules_.push_back({std::move(matches), std::move(action)});
        return *this;
    }

    // The first matching rule wins, even if its action is invalid. Rule
    // exceptions become fatal diagnostics; callbacks must not modify the
    // engine's rules during dispatch. The caller owns mutation and retry policy.
    LRDiagAction diagnose(const LRDiagState& state) const {
        if (!validState(state)) return fatal("invalid diagnostic state");
        for (const auto& rule : rules_) {
            try {
                if (!rule.matches(state)) continue;
                auto action = rule.action(state);
                // Fatal rules cannot edit input. Preserve their message and advice.
                if (action.severity == LRDiagSeverity::error) action.patch.reset();
                if (!validAction(state, action)) return fatal("invalid diagnostic action");
                return action;
            } catch (...) {
                return fatal("diagnostic rule threw an exception");
            }
        }
        return fatal(state.phase == LRDiagPhase::lexical ? "lexical error" : "syntax error");
    }

private:
    struct Rule {
        std::function<bool(const LRDiagState&)> matches;
        std::function<LRDiagAction(const LRDiagState&)> action;
    };

    std::vector<Rule> rules_;

    static LRDiagAction fatal(const char* message) {
        return {LRDiagSeverity::error, message, std::nullopt, {}};
    }

    static bool validRange(std::size_t begin, std::size_t end, std::size_t size) {
        return begin <= end && end <= size;
    }

    static bool terminal(const Grammar& grammar, Symbol symbol) {
        return symbol < grammar.symbols().size() && grammar.symbols()[symbol].terminal;
    }

    static bool inputTerminals(const Grammar& grammar, const std::vector<Symbol>& symbols) {
        for (const auto symbol : symbols)
            if (!terminal(grammar, symbol) || symbol == grammar.eof()) return false;
        return true;
    }

    static bool validState(const LRDiagState& state) {
        if (!validRange(state.byteBegin, state.byteEnd, state.source.size())) return false;
        const auto count = state.tokens ? state.tokens->size() : 0;
        if (state.tokenPosition > count) return false;
        switch (state.phase) {
        case LRDiagPhase::lexical:
            if (state.found || !state.expected.empty()) return false;
            return !state.grammar || !state.tokens || inputTerminals(*state.grammar, *state.tokens);
        case LRDiagPhase::syntactic: {
            if (!state.tokens || !state.grammar) return false;
            const auto& tokens = *state.tokens;
            if (state.found && state.tokenPosition < count && *state.found != tokens[state.tokenPosition])
                return false;
            const auto& grammar = *state.grammar;
            if (!inputTerminals(grammar, tokens)) return false;
            // Synthetic EOF is legal as lookahead, but never in the token vector.
            if (state.found && state.tokenPosition == count && *state.found != grammar.eof()) return false;
            for (const auto symbol : state.expected)
                if (!terminal(grammar, symbol)) return false;
            return true;
        }
        }
        return false;
    }

    static bool validAction(const LRDiagState& state, const LRDiagAction& action) {
        switch (action.severity) {
        case LRDiagSeverity::patch:
            // A patch-only action may be silent, but must supply an edit.
            if (!action.patch) return false;
            break;
        case LRDiagSeverity::inform:
        case LRDiagSeverity::warning:
        case LRDiagSeverity::error:
            if (action.message.empty()) return false;
            break;
        default:
            return false;
        }
        if (!action.patch) return true;
        if (const auto* patch = std::get_if<LRBytePatch>(&*action.patch))
            return state.phase == LRDiagPhase::lexical &&
                validRange(patch->begin, patch->end, state.source.size());
        if (const auto* patch = std::get_if<LRTokenPatch>(&*action.patch))
            return state.phase == LRDiagPhase::syntactic &&
                validRange(patch->begin, patch->end, state.tokens->size()) &&
                inputTerminals(*state.grammar, patch->replacement);
        return false;
    }
};

} // namespace firestone::lr
