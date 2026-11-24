#pragma once

#include "ATN.hpp"
#include "DFA.hpp"
#include "DFA2Assembly.hpp"
#include "DFA2C99.hpp"
#include "DFA2SIMD.hpp"
#include "DFAMinimization.hpp"
#include "DiagnosticsEngine.hpp"
#include "EBNF.hpp"
#include "GLL.hpp"
#include "GLR.hpp"
#include "LALR.hpp"
#include "LexerGenerator.hpp"
#include "LLFunctionDriven.hpp"
#include "LLTableDriven.hpp"
#include "NFA.hpp"
#include "ParserCombinator.hpp"
#include "ParserGenerator.hpp"
#include "PEG.hpp"
#include "RecursiveDescent.hpp"
#include "ThompsonConstruction.hpp"
#include "UTF8Automaton.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace firestone::lr {

// Bridge a byte lexer to any parser's token-vector API. Lexer rule IDs must
// already be Grammar terminal IDs. Skips and the synthetic EOF are not returned.
// A bad input byte returns its LexToken; an emitted nonterminal, EOF ID or
// unknown symbol is a lexer/grammar configuration error and throws.
inline std::variant<std::vector<Symbol>, LexToken> lexSymbols(
    const Lexer& lexer, const Grammar& grammar, std::string_view input) {
    std::vector<Symbol> symbols;
    std::size_t offset = 0;
    for (;;) {
        const auto token = lexer.next(input, offset);
        if (token.status == LexStatus::End) return symbols;
        if (token.status == LexStatus::Error) return token;
        if (token.token >= grammar.symbols().size() || !grammar.symbols()[token.token].terminal ||
            token.token == grammar.eof())
            throw std::invalid_argument("lexer emitted invalid grammar terminal "
                + std::to_string(token.token) + " at byte " + std::to_string(token.offset));
        symbols.push_back(token.token);
    }
}

} // namespace firestone::lr
