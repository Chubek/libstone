#include "aurocks/Compiler.hpp"

#include "EmitterRegex.hpp"
#include "firestone/liblr/ParserGenerator.hpp"

#include <chomsky3/chomsky3.h>
#include <chomsky3/regex.h>
#include <glr/grammar.h>
#include <glr/scannerless.h>
#include <regex.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace aurocks::detail {
namespace {

using Diagnostics = std::vector<Diagnostic>;

void error(Diagnostics& diagnostics, Location location, std::string code, std::string message) {
    diagnostics.push_back({Severity::error, location, std::move(code), std::move(message)});
}

bool hasErrors(const Diagnostics& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::error;
    });
}

std::string canonicalName(std::string value) {
    if (value == "lr(1)") return "lr1";
    if (value == "lalr(1)") return "lalr1";
    if (value == "slr(1)") return "slr1";
    return value;
}

template <typename Integer>
std::optional<Integer> parseInteger(std::string_view text) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() < '0' || text.front() > '9') return std::nullopt;
    }
    if (text.empty()) return std::nullopt;
    Integer value{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return value;
}

struct Settings {
    Analysis analysis;
    std::optional<std::size_t> expected;
    Location expectLocation;
    Location startLocation;
    Location skipLocation;
};

Settings readSettings(const Grammar& grammar, const Options& options, Diagnostics& diagnostics) {
    Settings settings;
    std::map<std::string, const Directive*> directives;
    for (const auto& directive : grammar.directives) {
        const auto name = directive.name == "algo" ? "algorithm" : directive.name;
        if (name == "prefer" || name == "priority") {
            diagnostics.push_back({Severity::warning, directive.location, "directive-scope",
                                   "global %" + directive.name + " is metadata only; place it on a production "
                                   "to affect disambiguation"});
            continue;
        }
        if (name != "backend" && name != "algorithm" && name != "disambiguator" &&
            name != "expect" && name != "start" && name != "skip") {
            continue;
        }
        if (!directives.emplace(name, &directive).second)
            error(diagnostics, directive.location, "duplicate-directive",
                  "duplicate %" + name + " directive");
    }

    const auto find = [&](const char* name) -> const Directive* {
        const auto it = directives.find(name);
        return it == directives.end() ? nullptr : it->second;
    };
    const auto location = [&](const char* name) -> Location {
        const auto* directive = find(name);
        return directive ? directive->location : Location{};
    };
    const auto choose = [&](const char* name, const std::string& override, const char* fallback) {
        const auto* directive = find(name);
        if (directive && directive->value.empty())
            error(diagnostics, directive->location, "missing-directive-value",
                  "%" + std::string(name) + " requires a value");
        if (!override.empty()) return override;
        return directive && !directive->value.empty() ? directive->value : std::string(fallback);
    };

    auto& analysis = settings.analysis;
    analysis.backend = choose("backend", options.backend, "portable");
    const bool portable = analysis.backend == "portable";
    const bool liblr = analysis.backend == "liblr";
    const bool libglr = analysis.backend == "libglr";
    if (!portable && !liblr && !libglr) {
        error(diagnostics, options.backend.empty() ? location("backend") : Location{}, "invalid-backend",
              "unknown backend '" + analysis.backend + "' (expected portable, liblr, or libglr)");
    }
    analysis.algorithm = canonicalName(choose("algorithm", options.algorithm, portable ? "earley" : "lr1"));
    const auto rejectAlgorithm = [&](const char* supported) {
        error(diagnostics, options.algorithm.empty() ? location("algorithm") : Location{}, "invalid-algorithm",
              "algorithm '" + analysis.algorithm + "' is not supported by " + analysis.backend +
                  " (expected " + supported + ")");
    };
    if (portable && analysis.algorithm != "earley") rejectAlgorithm("earley");
    if (liblr && analysis.algorithm != "lr1" && analysis.algorithm != "lalr1" && analysis.algorithm != "slr1")
        rejectAlgorithm("lr1, lalr1, or slr1");
    if (libglr && analysis.algorithm != "lr1") rejectAlgorithm("lr1");

    analysis.disambiguator = choose("disambiguator", options.disambiguator, "first");
    if (analysis.disambiguator != "first" && analysis.disambiguator != "error" &&
        analysis.disambiguator != "prefer" && analysis.disambiguator != "priority") {
        error(diagnostics, options.disambiguator.empty() ? location("disambiguator") : Location{},
              "invalid-disambiguator", "unknown disambiguator '" + analysis.disambiguator +
                  "' (expected first, error, prefer, or priority)");
    }

    if (const auto* expect = find("expect")) {
        settings.expected = parseInteger<std::size_t>(expect->value);
        settings.expectLocation = expect->location;
        if (!settings.expected)
            error(diagnostics, expect->location, "invalid-expect",
                  "%expect requires a nonnegative conflict count");
    }
    settings.startLocation = location("start");
    settings.skipLocation = location("skip");
    return settings;
}

void validateRuleDirectives(const Rule& rule, Diagnostics& diagnostics) {
    std::set<std::string> seen;
    for (const auto& directive : rule.directives) {
        if (directive.name == "prefer" || directive.name == "priority") {
            if (!seen.insert(directive.name).second)
                error(diagnostics, directive.location, "duplicate-directive",
                      "duplicate %" + directive.name + " on production");
            if (directive.name == "prefer" && !directive.value.empty())
                error(diagnostics, directive.location, "invalid-prefer", "%prefer does not take an argument");
            if (directive.name == "priority" && !parseInteger<int>(directive.value))
                error(diagnostics, directive.location, "invalid-priority", "%priority requires a signed integer");
        } else if (directive.name == "backend" || directive.name == "algorithm" || directive.name == "algo" ||
                   directive.name == "disambiguator" || directive.name == "expect" ||
                   directive.name == "start" || directive.name == "skip") {
            error(diagnostics, directive.location, "directive-scope",
                  "%" + directive.name + " belongs in the grammar declarations");
        }
    }
}

bool validateRegexSource(const std::string& pattern, std::string_view kind,
                         Location location, Diagnostics& diagnostics) {
    if (pattern.find('\0') != std::string::npos) {
        error(diagnostics, location, "invalid-regex", std::string(kind) + " must not contain NUL bytes");
        return false;
    }
    return true;
}

std::string chomskyRegexDetails(const std::string& pattern) {
    const std::unique_ptr<chomsky3_regex_t, decltype(&chomsky3_regex_free)> regex(
        chomsky3_regex_new(pattern.data(), pattern.size()), chomsky3_regex_free);
    if (!regex || regex->parse_error == CHOMSKY3_OK) return {};
    return " (libchomsky3 at pattern byte " + std::to_string(regex->parse_error_pos) + ": " +
        chomsky3_error_string(regex->parse_error) + ")";
}

void validatePortableRegex(portable::RegexProgram& program, const std::string& pattern,
                           Location location, std::string_view kind, Diagnostics& diagnostics) {
    try {
        program.add(pattern);
    } catch (const std::runtime_error& exception) {
        error(diagnostics, location, "invalid-regex", "invalid " + std::string(kind) + ": " +
                  exception.what() + chomskyRegexDetails(pattern));
    }
}

void validateGlrSkip(const std::string& pattern, Location location, Diagnostics& diagnostics) {
    regex_t compiled{};
    // Match the linked runtime's ERE syntax and flags. At runtime the
    // skip hook additionally requires a positive match at offset zero.
    if (const int status = regcomp(&compiled, pattern.c_str(), REG_EXTENDED)) {
        std::vector<char> message(regerror(status, &compiled, nullptr, 0));
        regerror(status, &compiled, message.data(), message.size());
        error(diagnostics, location, "invalid-regex",
              std::string("invalid %skip expression: ") + message.data() + chomskyRegexDetails(pattern));
        return;
    }
    regfree(&compiled);
}

void validateGrammar(const Grammar& grammar, const Settings& settings, Diagnostics& diagnostics) {
    if (grammar.rules.empty()) {
        error(diagnostics, settings.startLocation, "empty-grammar", "grammar has no productions");
        return;
    }
    std::set<std::string> definitions;
    for (const auto& rule : grammar.rules) definitions.insert(rule.lhs);
    if (!definitions.count(grammar.start))
        error(diagnostics, settings.startLocation, "undefined-start",
              "no productions for start symbol '" + grammar.start + "'");

    const bool linked = settings.analysis.backend == "libglr";
    portable::RegexProgram regexProgram;
    if (grammar.skip && validateRegexSource(*grammar.skip, "%skip expression", settings.skipLocation, diagnostics)) {
        if (linked) validateGlrSkip(*grammar.skip, settings.skipLocation, diagnostics);
        else validatePortableRegex(regexProgram, *grammar.skip, settings.skipLocation, "%skip expression", diagnostics);
    }
    std::set<std::string> checkedRegexes;
    for (const auto& rule : grammar.rules) {
        validateRuleDirectives(rule, diagnostics);
        for (const auto& symbol : rule.rhs) {
            switch (symbol.kind) {
            case SymbolKind::nonterminal:
                if (!definitions.count(symbol.text))
                    error(diagnostics, symbol.location, "undefined-nonterminal",
                          "no productions for nonterminal '" + symbol.text + "'");
                break;
            case SymbolKind::literal:
                if (symbol.text.empty() || symbol.text.find('\0') != std::string::npos)
                    error(diagnostics, symbol.location, "invalid-literal",
                          "terminal literals must not be empty or contain NUL bytes");
                break;
            case SymbolKind::regex:
                if (checkedRegexes.insert(symbol.text).second &&
                    validateRegexSource(symbol.text, "terminal regex", symbol.location, diagnostics) && !linked)
                    validatePortableRegex(regexProgram, symbol.text, symbol.location, "terminal regex", diagnostics);
                break;
            case SymbolKind::empty:
                if (rule.rhs.size() != 1)
                    error(diagnostics, symbol.location, "invalid-empty-production",
                          "an explicit empty symbol must be the sole symbol in its production");
                break;
            }
        }
    }
}

// Terminal identity includes its kind: a regex and a literal with equal
// spelling are still distinct lexer tokens. Internal names also cannot
// accidentally collide with the LR libraries' reserved end marker.
std::string terminalName(const Symbol& symbol) {
    return (symbol.kind == SymbolKind::regex ? "regex:" : "literal:") + symbol.text;
}

firestone::lr::Grammar toLiblr(const Grammar& grammar) {
    firestone::lr::Grammar adapted;
    for (const auto& rule : grammar.rules) adapted.nonterminal("nonterminal:" + rule.lhs);
    adapted.setStart(adapted.nonterminal("nonterminal:" + grammar.start));
    for (const auto& rule : grammar.rules) {
        std::vector<firestone::lr::Symbol> rhs;
        for (const auto& symbol : rule.rhs) {
            if (symbol.kind == SymbolKind::nonterminal)
                rhs.push_back(adapted.nonterminal("nonterminal:" + symbol.text));
            else if (symbol.kind != SymbolKind::empty)
                rhs.push_back(adapted.terminal(terminalName(symbol)));
        }
        adapted.addProduction(adapted.nonterminal("nonterminal:" + rule.lhs), std::move(rhs));
    }
    return adapted;
}

void analyzeLiblr(const Grammar& grammar, Analysis& analysis) {
    using firestone::lr::LRMode;
    const auto mode = analysis.algorithm == "slr1" ? LRMode::slr1
        : analysis.algorithm == "lalr1" ? LRMode::lalr1 : LRMode::canonicalLR1;
    const auto adapted = toLiblr(grammar);
    const auto table = firestone::lr::generateLRTable(adapted, mode);
    analysis.conflicts = table.conflicts().size();
    if (analysis.backend == "portable") {
        // Earley does not have a fixed automaton. Count the distinct dotted
        // production positions, not the input-dependent chart entries.
        analysis.states = 2; // Augmented start rule.
        for (const auto& production : adapted.productions())
            analysis.states += production.rhs.size() + 1;
    } else {
        analysis.states = table.stateCount();
    }
}

bool sameGlrAction(const glr_action_t& a, const glr_action_t& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case GLR_ACTION_SHIFT: return a.shift.next_state == b.shift.next_state;
    case GLR_ACTION_REDUCE: return a.reduce.production_id == b.reduce.production_id;
    case GLR_ACTION_ACCEPT:
    case GLR_ACTION_ERROR: return true;
    }
    return false;
}

std::size_t countGlrConflicts(const glr_parse_table_t& table) {
    std::size_t conflicts = 0;
    // The native builder can append the same shift once per contributing LR
    // item. Count distinct actions rather than the raw number of entries.
    for (std::uint32_t state = 0; state < table.state_count; ++state) {
        for (std::uint32_t terminal = 0; terminal < table.terminal_count; ++terminal) {
            const auto* cell = glr_parse_table_get_actions(&table, state, terminal);
            if (cell->action_count > 1 &&
                std::any_of(cell->actions + 1, cell->actions + cell->action_count, [&](const glr_action_t& action) {
                    return !sameGlrAction(cell->actions[0], action);
                })) {
                ++conflicts;
            }
        }
    }
    return conflicts;
}

void analyzeLibglr(const Grammar& grammar, Analysis& analysis, Diagnostics& diagnostics) {
    const std::unique_ptr<glr_grammar_t, decltype(&glr_grammar_destroy)> adapted(
        glr_grammar_create(), glr_grammar_destroy);
    if (!adapted) throw std::runtime_error("cannot allocate libglr grammar");

    std::map<std::string, int> nonterminals;
    std::map<std::string, int> terminals;
    const auto intern = [&](auto& ids, const std::string& name, glr_symbol_type_t type) {
        const auto [it, inserted] = ids.try_emplace(name, -1);
        if (inserted) it->second = glr_grammar_add_symbol(adapted.get(), type, name.c_str());
        if (it->second < 0) throw std::runtime_error("cannot add symbol to libglr grammar");
        return it->second;
    };
    for (const auto& rule : grammar.rules)
        intern(nonterminals, "nonterminal:" + rule.lhs, GLR_SYMBOL_NONTERMINAL);
    if (glr_grammar_set_start_symbol(adapted.get(), nonterminals.at("nonterminal:" + grammar.start)) != 0)
        throw std::runtime_error("cannot set libglr grammar start symbol");
    for (const auto& rule : grammar.rules) {
        std::vector<glr_symbol_t*> rhs;
        for (const auto& symbol : rule.rhs) {
            if (symbol.kind == SymbolKind::empty) continue;
            if (symbol.kind == SymbolKind::nonterminal) {
                rhs.push_back(glr_grammar_get_symbol(adapted.get(), nonterminals.at("nonterminal:" + symbol.text)));
                continue;
            }
            const auto name = terminalName(symbol);
            const bool newTerminal = terminals.find(name) == terminals.end();
            const int id = intern(terminals, name, GLR_SYMBOL_TERMINAL);
            if (newTerminal && symbol.kind == SymbolKind::regex) {
                char message[256]{};
                if (glr_scannerless_set_pattern(adapted.get(), id, symbol.text.c_str(), message, sizeof(message)) != 0) {
                    error(diagnostics, symbol.location, "invalid-regex",
                          std::string("invalid libglr terminal regex: ") + message + chomskyRegexDetails(symbol.text));
                }
            }
            rhs.push_back(glr_grammar_get_symbol(adapted.get(), id));
        }
        if (glr_grammar_add_production(adapted.get(), nonterminals.at("nonterminal:" + rule.lhs),
                                       rhs.data(), rhs.size()) < 0)
            throw std::runtime_error("cannot add production to libglr grammar");
    }
    if (hasErrors(diagnostics)) return;

    // The public header's SLR comment is stale: the vendored implementation
    // keys states by complete LR(1) item sets, including their lookaheads.
    char message[256]{};
    const std::unique_ptr<glr_parse_table_t, decltype(&glr_parse_table_destroy)> table(
        glr_grammar_build_parse_table(adapted.get(), message, sizeof(message)), glr_parse_table_destroy);
    if (!table) throw std::runtime_error(message[0] ? message : "cannot build libglr parse table");
    analysis.states = table->state_count;
    analysis.conflicts = countGlrConflicts(*table);
}

} // namespace

Analysis analyze(const Grammar& grammar, const Options& options, std::vector<Diagnostic>& diagnostics) {
    auto settings = readSettings(grammar, options, diagnostics);
    validateGrammar(grammar, settings, diagnostics);
    auto& analysis = settings.analysis;
    if (hasErrors(diagnostics)) return std::move(analysis);

    try {
        if (analysis.backend == "libglr") analyzeLibglr(grammar, analysis, diagnostics);
        else analyzeLiblr(grammar, analysis);
    } catch (const std::exception& exception) {
        error(diagnostics, settings.startLocation, "backend-analysis",
              "cannot analyze grammar for " + analysis.backend + ": " + exception.what());
        return std::move(analysis);
    }
    if (!hasErrors(diagnostics) && settings.expected && *settings.expected != analysis.conflicts) {
        const auto algorithm = analysis.backend == "portable" ? "lr1" : analysis.algorithm;
        error(diagnostics, settings.expectLocation, "conflict-count",
              "%expect " + std::to_string(*settings.expected) + ", but " + algorithm + " analysis found " +
                  std::to_string(analysis.conflicts) + " conflicting action cells");
    }
    return std::move(analysis);
}

} // namespace aurocks::detail
