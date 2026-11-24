#include "aurocks/API.h"
#include "aurocks/Compiler.hpp"
#include "Emitter.hpp"

#include <algorithm>
#include <memory>

namespace aurocks {
namespace {

// The original scanner gives \n and friends byte meanings even in bracket
// expressions. Convert these before passing the expression to a POSIX engine.
std::string normalizeRegex(std::string_view text) {
    std::string out;
    std::size_t bracket = std::string_view::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\\' && i + 1 < text.size()) {
            const char escaped = text[++i];
            switch (escaped) {
            case 'a': out += '\a'; continue;
            case 'b':
                if (bracket == std::string_view::npos) out += "\\b";
                else out += '\b';
                continue;
            case 'B':
                if (bracket == std::string_view::npos) out += "\\B";
                else out += 'B';
                continue;
            case 'e': out += '\x1b'; continue;
            case 'f': out += '\f'; continue;
            case 'n': out += '\n'; continue;
            case 'r': out += '\r'; continue;
            case 't': out += '\t'; continue;
            case 'v': out += '\v'; continue;
            case '0': out += '\0'; continue;
            case '/': out += '/'; continue;
            default: break;
            }
            if (bracket == std::string_view::npos) {
                switch (escaped) {
                case 'd': out += "[[:digit:]]"; continue;
                case 'D': out += "[^[:digit:]]"; continue;
                case 's': out += "[[:space:]]"; continue;
                case 'S': out += "[^[:space:]]"; continue;
                case 'w': out += "[[:alnum:]_]"; continue;
                case 'W': out += "[^[:alnum:]_]"; continue;
                default: break;
                }
            }
            if (std::string_view(R"(.^$[]()*+?{}|-\)").find(escaped) != std::string_view::npos)
                out += '\\';
            out += escaped;
            continue;
        }
        if (ch == '[') {
            if (bracket == std::string_view::npos) {
                bracket = i + 1;
                if (bracket < text.size() && text[bracket] == '^') ++bracket;
            } else if (i + 1 < text.size() && std::string_view(":.=").find(text[i + 1]) !=
                                                       std::string_view::npos) {
                const char suffix[] = {text[i + 1], ']'};
                const auto end = text.find(std::string_view(suffix, 2), i + 2);
                if (end != std::string_view::npos) {
                    out += text.substr(i, end + 2 - i);
                    i = end + 1;
                    continue;
                }
            }
        } else if (ch == ']' && bracket != std::string_view::npos && i != bracket) {
            bracket = std::string_view::npos;
        }
        out += ch;
    }
    return out;
}

void preparePatterns(Grammar& grammar, std::vector<Diagnostic>& diagnostics) {
    const auto prepare = [&](std::string& text, Location location) {
        text = normalizeRegex(text);
        if (text.find('\0') != std::string::npos)
            diagnostics.push_back({Severity::error, location, "regex-nul",
                                   "a text regular expression cannot contain a NUL byte"});
    };
    if (grammar.skip) {
        const auto it = std::find_if(grammar.directives.begin(), grammar.directives.end(),
                                     [](const auto& d) { return d.name == "skip"; });
        prepare(*grammar.skip, it == grammar.directives.end() ? Location{} : it->location);
    }
    for (auto& rule : grammar.rules)
        for (auto& symbol : rule.rhs)
            if (symbol.kind == SymbolKind::regex) prepare(symbol.text, symbol.location);
}

} // namespace

bool Result::ok() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.severity == Severity::error; });
}

Result compile(std::string_view source, const Options& options) {
    Result result;
    if (const auto nul = source.find('\0'); nul != std::string_view::npos) {
        Location location;
        location.offset = nul;
        for (std::size_t p = 0; p < nul; ++p) {
            if (source[p] == '\r' || (source[p] == '\n' && (p == 0 || source[p - 1] != '\r'))) {
                ++location.line;
                location.column = 1;
            } else if (source[p] != '\n') {
                ++location.column;
            }
        }
        result.diagnostics.push_back({Severity::error, location, "syntax.nul",
                                      "embedded NUL byte in grammar source"});
        return result;
    }
    auto grammar = detail::parse(source, result.diagnostics);
    if (result.ok())
        detail::compileFaultRules(grammar, result.diagnostics);

    auto effective = options;
    if (result.ok())
        detail::applyExtensions(grammar, effective, result.diagnostics);
    if (result.ok()) {
        const auto canonical = "parse_" + grammar.start;
        if (effective.entrypoint == "main" ||
            (!effective.entrypoint.empty() && effective.entrypoint + "_error" == canonical))
            result.diagnostics.push_back({Severity::error, {}, "invalid-entrypoint",
                                          "entry point must be an available C identifier"});
    }
    if (result.ok()) {
        const auto profile = std::find_if(grammar.directives.begin(), grammar.directives.end(),
                                          [](const auto& d) { return d.name == "backend"; });
        if (effective.backend == "libglr" ||
            (effective.backend.empty() && profile != grammar.directives.end() && profile->value == "libglr"))
            preparePatterns(grammar, result.diagnostics);
    }
    if (result.ok())
        result.analysis = detail::analyze(grammar, effective, result.diagnostics);
    if (result.ok()) {
        if (effective.check_only)
            detail::validateEmission(grammar, result.analysis, effective.entrypoint, result.diagnostics);
        else
            result.source = detail::emit(grammar, result.analysis, effective.entrypoint, result.diagnostics);
    }
    if (!result.ok())
        result.source.clear();
    return result;
}

} // namespace aurocks

struct aurocks_result {
    aurocks::Result value;
};

extern "C" aurocks_result *aurocks_compile(const char *grammar, size_t length,
                                          const aurocks_options *input) {
    if (!grammar)
        return nullptr;
    try {
        aurocks::Options options;
        if (input) {
            if (input->entrypoint) options.entrypoint = input->entrypoint;
            if (input->backend) options.backend = input->backend;
            if (input->algorithm) options.algorithm = input->algorithm;
            if (input->disambiguator) options.disambiguator = input->disambiguator;
            if (input->lua_source) options.lua_source = input->lua_source;
            options.check_only = input->check_only != 0;
        }
        auto result = std::make_unique<aurocks_result>();
        result->value = aurocks::compile(std::string_view(grammar, length), options);
        return result.release();
    } catch (...) {
        // Never let a C++ exception cross the C linkage boundary.
        return nullptr;
    }
}

extern "C" void aurocks_result_destroy(aurocks_result *result) {
    delete result;
}

extern "C" int aurocks_result_ok(const aurocks_result *result) {
    return result && result->value.ok();
}

extern "C" const char *aurocks_result_source(const aurocks_result *result, size_t *length) {
    if (length) *length = result ? result->value.source.size() : 0;
    return result ? result->value.source.c_str() : nullptr;
}

extern "C" size_t aurocks_result_diagnostic_count(const aurocks_result *result) {
    return result ? result->value.diagnostics.size() : 0;
}

extern "C" int aurocks_result_analysis(const aurocks_result *result, aurocks_analysis *out) {
    if (!result || !out || result->value.analysis.backend.empty()) return 0;
    const auto& a = result->value.analysis;
    *out = {a.backend.c_str(), a.algorithm.c_str(), a.disambiguator.c_str(), a.states, a.conflicts};
    return 1;
}

extern "C" int aurocks_result_diagnostic(const aurocks_result *result, size_t index,
                                        aurocks_diagnostic *out) {
    if (!result || !out || index >= result->value.diagnostics.size())
        return 0;
    const auto& d = result->value.diagnostics[index];
    *out = {d.severity == aurocks::Severity::error ? AUROCKS_ERROR : AUROCKS_WARNING,
            d.location.offset, d.location.line, d.location.column, d.code.c_str(), d.message.c_str()};
    return 1;
}
