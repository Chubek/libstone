#pragma once
#include "diagnostic.hpp"
#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace smtlib2 {
enum class TokenKind {
    lparen, rparen, atom, string_literal, quoted_symbol,
    numeral, decimal, binary, hexadecimal, keyword, reserved
};
struct Token { TokenKind kind; std::string text; Span span; };
struct Limits { std::size_t max_input_bytes = 4 * 1024 * 1024, max_tokens = 1000000, max_depth = 512; };

enum class CommandKind {
    assert_, check_sat, check_sat_assuming, declare_const, declare_datatype,
    declare_datatypes, declare_fun, declare_sort, define_fun, define_fun_rec,
    define_funs_rec, define_sort, echo, exit, get_assertions, get_assignment,
    get_info, get_model, get_option, get_proof, get_unsat_assumptions,
    get_unsat_core, get_value, pop, push, reset, reset_assertions, set_info,
    set_logic, set_option
};

namespace detail {
// Sorted by spelling for use in the lexer and the typed script parser.
struct CommandSpec { std::string_view spelling; CommandKind kind; std::size_t arity; };
inline constexpr CommandSpec commands[] = {
    {"assert", CommandKind::assert_, 1}, {"check-sat", CommandKind::check_sat, 0},
    {"check-sat-assuming", CommandKind::check_sat_assuming, 1}, {"declare-const", CommandKind::declare_const, 2},
    {"declare-datatype", CommandKind::declare_datatype, 2}, {"declare-datatypes", CommandKind::declare_datatypes, 2},
    {"declare-fun", CommandKind::declare_fun, 3}, {"declare-sort", CommandKind::declare_sort, 2},
    {"define-fun", CommandKind::define_fun, 4}, {"define-fun-rec", CommandKind::define_fun_rec, 4},
    {"define-funs-rec", CommandKind::define_funs_rec, 2}, {"define-sort", CommandKind::define_sort, 3},
    {"echo", CommandKind::echo, 1}, {"exit", CommandKind::exit, 0}, {"get-assertions", CommandKind::get_assertions, 0},
    {"get-assignment", CommandKind::get_assignment, 0}, {"get-info", CommandKind::get_info, 1},
    {"get-model", CommandKind::get_model, 0}, {"get-option", CommandKind::get_option, 1},
    {"get-proof", CommandKind::get_proof, 0}, {"get-unsat-assumptions", CommandKind::get_unsat_assumptions, 0},
    {"get-unsat-core", CommandKind::get_unsat_core, 0}, {"get-value", CommandKind::get_value, 1},
    {"pop", CommandKind::pop, 1}, {"push", CommandKind::push, 1}, {"reset", CommandKind::reset, 0},
    {"reset-assertions", CommandKind::reset_assertions, 0}, {"set-info", CommandKind::set_info, 2},
    {"set-logic", CommandKind::set_logic, 1}, {"set-option", CommandKind::set_option, 2}
};
inline const CommandSpec* find_command(std::string_view text) {
    const auto* end = commands + sizeof(commands) / sizeof(*commands);
    const auto* match = std::lower_bound(commands, end, text,
        [](const CommandSpec& command, std::string_view spelling) { return command.spelling < spelling; });
    return match != end && match->spelling == text ? match : nullptr;
}
inline bool lex_digit(char c) { return c >= '0' && c <= '9'; }
inline bool lex_symbol_char(char c) {
    return lex_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || std::string_view("~!@$%^&*_-+=<>.?/").find(c) != std::string_view::npos;
}
inline bool lex_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool lex_text_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (byte >= 0x20 && byte <= 0x7e) || byte >= 0x80 || lex_space(c);
}
inline TokenKind lex_bare(std::string_view text, Span span) {
    auto invalid = [&] { return Error(span, "invalid SMT-LIB token", ErrorCategory::lexical); };
    const char first = text.front();
    if (first == '#') {
        if (text.size() < 3 || (text[1] != 'b' && text[1] != 'x')) throw invalid();
        const bool binary = text[1] == 'b';
        for (char c : text.substr(2)) {
            if (binary ? c != '0' && c != '1'
                       : !lex_digit(c) && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F'))
                throw invalid();
        }
        return binary ? TokenKind::binary : TokenKind::hexadecimal;
    }
    if (lex_digit(first)) {
        std::size_t n = 1;
        while (n < text.size() && lex_digit(text[n])) ++n;
        if (first == '0' && n != 1) throw invalid();
        if (n == text.size()) return TokenKind::numeral;
        if (text[n] != '.' || ++n == text.size()) throw invalid();
        for (char c : text.substr(n)) if (!lex_digit(c)) throw invalid();
        return TokenKind::decimal;
    }
    const bool keyword = first == ':';
    auto name = text.substr(keyword ? 1 : 0);
    if (name.empty() || lex_digit(name.front())) throw invalid();
    for (char c : name) if (!lex_symbol_char(c)) throw invalid();
    bool reserved = find_command(name) != nullptr;
    for (std::string_view word : {"BINARY", "DECIMAL", "HEXADECIMAL", "NUMERAL", "STRING",
                                  "_", "!", "as", "let", "exists", "forall", "match", "par"}) {
        if (name == word) { reserved = true; break; }
    }
    if (keyword) {
        if (reserved) throw invalid(); // A keyword suffix must be a simple symbol.
        return TokenKind::keyword;
    }
    return reserved ? TokenKind::reserved : TokenKind::atom;
}
}
/// @implstring Recognize SMT-LIB 2.6 token categories and preserve exact
/// spelling and half-open byte spans. Standard command spellings are reserved.
/// Preserve non-ASCII bytes in delimited text without choosing or validating a
/// particular character encoding. Strings use doubled quotes, and quoted
/// symbols have no escapes.
inline std::vector<Token> lex(std::string_view input, Limits limits = {}) {
    if (input.size() > limits.max_input_bytes)
        throw Error({}, "input size limit exceeded", ErrorCategory::resource_limit);
    std::vector<Token> out; Position p;
    auto advance = [&] {
        char c = input[p.offset++];
        if (c == '\r' || c == '\n') {
            if (c != '\n' || p.offset == 1 || input[p.offset - 2] != '\r') ++p.line;
            p.column = 1;
        } else ++p.column;
    };
    auto validate_text = [&](const char* message) {
        Position begin = p;
        advance();
        throw Error({begin, p}, message, ErrorCategory::lexical);
    };
    while (p.offset < input.size()) {
        char c = input[p.offset];
        if (detail::lex_space(c)) { advance(); continue; }
        if (c == ';') {
            while (p.offset < input.size() && input[p.offset] != '\n' && input[p.offset] != '\r')
                advance();
            continue;
        }
        if (out.size() >= limits.max_tokens)
            throw Error({p,p}, "token limit exceeded", ErrorCategory::resource_limit);
        Position start = p; TokenKind kind = TokenKind::atom;
        if (c == '(' || c == ')') { kind = c == '(' ? TokenKind::lparen : TokenKind::rparen; advance(); }
        else if (c == '"') {
            kind = TokenKind::string_literal; advance(); bool closed = false;
            while (p.offset < input.size()) {
                if (input[p.offset] == '"') { advance(); if (p.offset < input.size() && input[p.offset] == '"') { advance(); continue; } closed = true; break; }
                if (!detail::lex_text_char(input[p.offset])) validate_text("invalid character in string literal");
                advance();
            }
            if (!closed) throw Error({start,p}, "unterminated string literal", ErrorCategory::lexical);
        } else if (c == '|') {
            kind = TokenKind::quoted_symbol; advance(); bool closed = false;
            while (p.offset < input.size()) {
                c = input[p.offset];
                if (c == '|') { advance(); closed = true; break; }
                if (c == '\\') validate_text("backslash in quoted symbol");
                if (!detail::lex_text_char(c)) validate_text("invalid character in quoted symbol");
                advance();
            }
            if (!closed) throw Error({start,p}, "unterminated quoted symbol", ErrorCategory::lexical);
        } else {
            while (p.offset < input.size() && !detail::lex_space(input[p.offset]) && input[p.offset] != '(' && input[p.offset] != ')' && input[p.offset] != ';') {
                if (input[p.offset] == '"' || input[p.offset] == '|') validate_text("invalid character in atom");
                advance();
            }
            kind = detail::lex_bare(input.substr(start.offset, p.offset - start.offset), {start,p});
        }
        out.push_back({kind, std::string(input.substr(start.offset, p.offset-start.offset)), {start,p}});
    }
    return out;
}
}
