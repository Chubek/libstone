#pragma once

#include "DiagnosticsBytePEG.hpp"
#include "DiagnosticsCore.hpp"
#include "DFA.hpp"
#include "PEG.hpp"
#include "ThompsonConstruction.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace firestone::lr {

// Diagnostic predicates are anchored prefix matches, not searches. Lexical
// predicates start at byteBegin and may inspect beyond byteEnd. Unlike lexer
// rules, successful empty matches count, including at actual EOF. An anchor
// past the input never matches.
inline auto lexicalLiteral(std::string literal) {
    return [literal = std::move(literal)](const LRDiagState& state) {
        return state.phase == LRDiagPhase::lexical && state.byteBegin <= state.source.size() &&
            state.source.substr(state.byteBegin, literal.size()) == literal;
    };
}

// Compiles the liblr byte-regex syntax once; syntax errors throw RegexError
// at construction. There are no regex anchors (including an EOF anchor).
inline auto lexicalRegex(std::string_view pattern) {
    return [dfa = determinize(compile_regex(pattern))](const LRDiagState& state) {
        return state.phase == LRDiagPhase::lexical && state.byteBegin <= state.source.size() &&
            dfa.match_prefix(state.source.substr(state.byteBegin)).has_value();
    };
}

inline auto lexicalPEG(LRDiagBytePEG peg) {
    return [peg = std::move(peg)](const LRDiagState& state) {
        return state.phase == LRDiagPhase::lexical &&
            peg.matchPrefixAt(state.source, state.byteBegin).has_value();
    };
}

// The caller supplies the grammar that assigned the IDs in tokens. Every
// input token must name the same terminal at the same ID in both grammars.
// Productions and nonterminal IDs need not agree. Missing/incompatible
// state does not match. Do not append an EOF token: end() matches at the
// true end of the vector, and a successful zero-width PEG counts.
inline auto syntacticPEG(PEG peg) {
    return [peg = std::move(peg)](const LRDiagState& state) {
        if (state.phase != LRDiagPhase::syntactic || !state.grammar || !state.tokens ||
            state.tokenPosition > state.tokens->size()) return false;
        const auto& sourceSymbols = state.grammar->symbols();
        const auto& patternSymbols = peg.grammar().symbols();
        for (const auto id : *state.tokens) {
            if (id == state.grammar->eof() || id == peg.grammar().eof() ||
                id >= sourceSymbols.size() || id >= patternSymbols.size() ||
                !sourceSymbols[id].terminal || !patternSymbols[id].terminal ||
                sourceSymbols[id].name != patternSymbols[id].name) return false;
        }
        return peg.matchPrefixAt(*state.tokens, state.tokenPosition).has_value();
    };
}

using LRDiagByteMap = std::array<std::optional<Symbol>, byte_count>;

// Map each unsigned byte to a non-EOF terminal of the PEG's grammar.
// If ANY byte in the suffix is unmapped, the predicate fails rather than
// dropping bytes or treating the first unmapped byte as EOF. To match a
// prefix regardless of trailing bytes, map those bytes to an ordinary
// "other" terminal. At true EOF the suffix is empty, so end() can succeed.
inline auto lexicalPEG(PEG peg, LRDiagByteMap mapping) {
    const auto& grammar = peg.grammar();
    for (const auto id : mapping) {
        if (id && (*id == grammar.eof() || *id >= grammar.symbols().size() ||
                   !grammar.symbols()[*id].terminal))
            throw std::invalid_argument("lexical PEG byte mapping requires a non-EOF terminal");
    }
    return [peg = std::move(peg), mapping = std::move(mapping)](const LRDiagState& state) {
        if (state.phase != LRDiagPhase::lexical || state.byteBegin > state.source.size()) return false;
        std::vector<Symbol> tokens;
        for (const unsigned char byte : state.source.substr(state.byteBegin)) {
            const auto id = mapping[byte];
            if (!id) return false;
            tokens.push_back(*id);
        }
        return peg.matchPrefix(tokens).has_value();
    };
}

} // namespace firestone::lr
