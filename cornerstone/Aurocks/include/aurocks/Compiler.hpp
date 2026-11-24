#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurocks {

struct Location {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

enum class Severity { warning, error };

struct Diagnostic {
    Severity severity;
    Location location;
    std::string code;
    std::string message;
};

struct Directive {
    std::string name;  // Without '%'.
    std::string value; // Raw arguments, trimmed.
    Location location;
};

enum class SymbolKind { nonterminal, literal, regex, empty };

struct Symbol {
    SymbolKind kind;
    std::string text; // Decoded literal or ERE contents, without delimiters.
    Location location;
};

struct Rule {
    std::string lhs;
    std::vector<Symbol> rhs;
    std::string action; // C fragment, without outer braces.
    std::vector<Directive> directives;
    Location location;
};

struct Grammar {
    std::string prologue;
    std::string epilogue;
    std::string start;
    std::optional<std::string> skip;
    std::vector<Directive> directives;
    std::vector<Rule> rules;
};

struct Options {
    std::string entrypoint;
    std::string backend;
    std::string algorithm;
    std::string disambiguator;
    bool check_only = false;
    // Lua source is supplied explicitly by the host; grammar text never loads files.
    std::optional<std::string> lua_source;
};

struct Analysis {
    std::string backend;
    std::string algorithm;
    std::string disambiguator;
    std::size_t states = 0;
    std::size_t conflicts = 0;
};

struct Result {
    std::string source;
    Analysis analysis;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool ok() const;
};

Result compile(std::string_view source, const Options& options = {});

// Implementation seams shared by the frontend, backends and emitters.
namespace detail {
std::optional<Grammar> parse(std::string_view source, std::vector<Diagnostic>& diagnostics);
Analysis analyze(const Grammar& grammar, const Options& options,
                 std::vector<Diagnostic>& diagnostics);
std::string emit(const Grammar& grammar, const Analysis& analysis,
                 std::string_view entrypoint, std::vector<Diagnostic>& diagnostics);
void applyExtensions(Grammar& grammar, Options& options, std::vector<Diagnostic>& diagnostics);
void applyFaultPatterns(const Grammar& grammar, std::vector<Diagnostic>& diagnostics);
}

} // namespace aurocks
