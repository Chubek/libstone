#include "Emitter.hpp"
#include "EmitterHelpers.hpp"
#include "EmitterRegex.hpp"
#include "EmitterRuntime.hpp"
#include "LinkedEmitter.hpp"

#include <algorithm>
#include <charconv>
#include <ostream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace aurocks::detail {

namespace {

using Diagnostics = std::vector<Diagnostic>;

void error(Diagnostics& diagnostics, Location at, std::string code, std::string message) {
    diagnostics.push_back({Severity::error, at, std::move(code), std::move(message)});
}

bool alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool identifierByte(char c) { return alpha(c) || digit(c); }

bool validEntrypoint(std::string_view s) {
    static const std::set<std::string_view> keywords{
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double",
        "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
        "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
        "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Bool",
        "_Complex", "_Imaginary", "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn",
        "_Static_assert", "_Thread_local", "NULL", "size_t",
    };
    static const std::set<std::string_view> internal{
        "my_strdup", "aurocks_action", "aurocks_ambiguity_error", "aurocks_arg", "aurocks_box_double",
        "aurocks_column", "aurocks_edge", "aurocks_error_position", "aurocks_evaluate", "aurocks_find", "aurocks_frame",
        "aurocks_grow", "aurocks_has_skip", "aurocks_item", "aurocks_parse_glr", "aurocks_parse_input",
        "aurocks_parser", "aurocks_predict", "aurocks_priority", "aurocks_production_ids", "aurocks_productions",
        "aurocks_recognize", "aurocks_record", "aurocks_rehash", "aurocks_resolve", "aurocks_rhs",
        "aurocks_rule", "aurocks_rules", "aurocks_rx_match", "aurocks_rx_push", "aurocks_rx_state",
        "aurocks_scan", "aurocks_sets", "aurocks_skip", "aurocks_skip_rx", "aurocks_slot",
        "aurocks_start", "aurocks_states", "aurocks_step", "aurocks_symbol", "aurocks_symbols",
        "aurocks_terminal_use", "aurocks_visit", "aurocks_word_byte", "yyparse_error",
        "AUROCKS_EDGE_LIMIT", "AUROCKS_INPUT_LIMIT",
        "AUROCKS_ITEM_LIMIT", "AUROCKS_MAX_RHS", "AUROCKS_RULES", "AUROCKS_RX_STATES", "AUROCKS_WORK_LIMIT",
        "AUROCKS_RUNTIME_FUNCTION_COUNT", "AUROCKS_THREAD_LOCAL", "aurocks_runtime_functions",
        "aurocks_alloc", "aurocks_calloc", "aurocks_realloc", "aurocks_free", "aurocks_strdup",
        "aurocks_strndup", "aurocks_memdup", "aurocks_strlen", "aurocks_strcmp", "aurocks_strncmp",
        "aurocks_strcasecmp", "aurocks_strchr", "aurocks_strrchr", "aurocks_strstr", "aurocks_strlcpy",
        "aurocks_strtol", "aurocks_strtoul", "aurocks_strtoll", "aurocks_strtoull",
        "aurocks_strtod", "aurocks_strtof", "aurocks_strtold", "aurocks_atoi",
        "aurocks_atol", "aurocks_atoll", "aurocks_atof", "aurocks_snprintf", "aurocks_vsnprintf",
        "aurocks_asprintf", "aurocks_vasprintf", "aurocks_unbox_double", "aurocks_box_long",
        "aurocks_unbox_long", "aurocks_box_str",
    };
    return !s.empty() && alpha(s.front()) && std::all_of(s.begin(), s.end(), identifierByte) &&
           !keywords.count(s) && !internal.count(s) && !s.starts_with("aurocks_rhs_");
}

Location locationAt(Location initial, std::string_view text) {
    for (char c : text) {
        ++initial.offset;
        if (c == '\n') {
            ++initial.line;
            initial.column = 1;
        } else {
            ++initial.column;
        }
    }
    return initial;
}

template <typename Integer>
bool integer(std::string_view text, Integer& value) {
    if (text.empty())
        return false;
    if (text.front() == '+')
        text.remove_prefix(1);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

struct Token {
    enum Kind { trivia, opaque, name, reference, punctuation } kind;
    std::string_view text;
    std::size_t offset;
    std::string replacement, before, after;
    std::size_t valueIndex = 0;
};

std::vector<Token> tokenize(std::string_view code) {
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < code.size();) {
        const auto start = i;
        const auto c = code[i++];
        auto kind = Token::punctuation;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            kind = Token::trivia;
        } else if (c == '/' && i < code.size() && (code[i] == '/' || code[i] == '*')) {
            kind = Token::trivia;
            if (code[i++] == '*') {
                const auto close = code.find("*/", i);
                i = close == std::string_view::npos ? code.size() : close + 2;
            } else {
                while (i < code.size() && code[i] != '\n') {
                    if (code[i++] == '\\' && i < code.size() && code[i] == '\n')
                        ++i;
                }
            }
        } else if (c == '"' || c == '\'') {
            kind = Token::opaque;
            while (i < code.size()) {
                const auto ch = code[i++];
                if (ch == c)
                    break;
                if (ch == '\\' && i < code.size())
                    ++i;
            }
        } else if (c == '$') {
            kind = Token::reference;
            if (code.substr(i, 7) == "{TOKEN}")
                i += 7;
            else if (i < code.size() && code[i] == '$')
                ++i;
            else
                while (i < code.size() && identifierByte(code[i]))
                    ++i;
        } else if (alpha(c)) {
            kind = Token::name;
            while (i < code.size() && identifierByte(code[i]))
                ++i;
        } else if (c == '=' && i < code.size() && code[i] == '=') {
            ++i;
        }
        tokens.push_back({kind, code.substr(start, i - start), start, {}, {}, {}, 0});
    }
    return tokens;
}

struct Action {
    std::string code;
    std::vector<unsigned> terminalUse;
    std::vector<std::size_t> usedValues;
    bool strdup = false;
};

bool terminal(SymbolKind kind) { return kind == SymbolKind::literal || kind == SymbolKind::regex; }

Action transform(const Rule& rule, bool legacySList, Diagnostics& diagnostics) {
    Action action;
    action.terminalUse.resize(rule.rhs.size());
    auto tokens = tokenize(rule.action);
    std::vector<std::size_t> significant;
    bool usesToken = false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        auto& tok = tokens[i];
        if (tok.kind == Token::trivia)
            continue;
        const bool member = (!significant.empty() && tokens[significant.back()].text == ".") ||
                            (significant.size() >= 2 && tokens[significant.back()].text == ">" &&
                             tokens[significant[significant.size() - 2]].text == "-");
        significant.push_back(i);
        if (member && tok.kind == Token::name)
            continue;
        if (tok.kind == Token::reference || tok.kind == Token::name) {
            if (tok.text == "$$") {
                tok.replacement = "result";
            } else if (tok.text == "$TOKEN" || tok.text == "${TOKEN}" || tok.text == "token") {
                tok.replacement = "token";
                usesToken = true;
            } else if (tok.text == "my_strdup") {
                action.strdup = true;
            } else if (tok.kind == Token::reference ||
                       (tok.text.size() > 1 && tok.text.front() == 'v' &&
                        std::all_of(tok.text.begin() + 1, tok.text.end(), digit))) {
                const auto index = tok.text.substr(1);
                std::size_t n = 0;
                if (index.empty() || !std::all_of(index.begin(), index.end(), digit) ||
                    !integer(index, n) || !n || n > rule.rhs.size() ||
                    rule.rhs[n - 1].kind == SymbolKind::empty) {
                    if (tok.kind == Token::reference)
                        error(diagnostics, locationAt(rule.location, rule.action.substr(0, tok.offset)),
                              "invalid-action-reference", "invalid action reference '" + std::string(tok.text) + "'");
                } else {
                    tok.valueIndex = n;
                    tok.replacement = "v" + std::to_string(n);
                    action.usedValues.push_back(n);
                    if (terminal(rule.rhs[n - 1].kind))
                        action.terminalUse[n - 1] |= 1;
                }
            }
        }
    }
    if (usesToken) {
        for (auto i = rule.rhs.size(); i > 0; --i) {
            if (terminal(rule.rhs[i - 1].kind)) {
                action.terminalUse[i - 1] |= 2;
                break;
            }
        }
    }
    std::sort(action.usedValues.begin(), action.usedValues.end());
    action.usedValues.erase(std::unique(action.usedValues.begin(), action.usedValues.end()),
                            action.usedValues.end());

    const auto at = [&](std::size_t i) -> Token& { return tokens[significant[i]]; };
    bool warnedReturn = false;
    for (std::size_t i = 0; i + 2 < significant.size(); ++i) {
        if (at(i).text == "attempt" && at(i + 1).text == ".") {
            error(diagnostics, locationAt(rule.location, rule.action.substr(0, at(i).offset)),
                  "unsupported-action", "the legacy mutable 'attempt' parser state is not available in semantic actions");
        }
        if (!warnedReturn && at(i).text == "return" &&
            (at(i + 1).text == "0" || at(i + 1).text == "NULL") && at(i + 2).text == ";") {
            diagnostics.push_back({Severity::warning, locationAt(rule.location, rule.action.substr(0, at(i).offset)),
                                   "action-return", "returning NULL from an action supplies a semantic value; "
                                                    "it does not reject this parse or backtrack"});
            warnedReturn = true;
        }
        if (legacySList && at(i).valueIndex && at(i + 1).text == "-" && at(i + 2).text == ">") {
            at(i).before += "((SList *)";
            at(i).after += ')';
        }
        if (at(i).text == "build_number" && at(i + 1).text == "(" &&
            at(i + 2).valueIndex && i + 3 < significant.size() && at(i + 3).text == ")") {
            // Compatibility adapter from the Perl generator's boxed JSON values.
            at(i + 2).before += "*(double *)";
        }
        const bool member = (i && at(i - 1).text == ".") ||
                            (i >= 2 && at(i - 2).text == "-" && at(i - 1).text == ">");
        if ((at(i).text != "$$" && (at(i).text != "result" || member)) || at(i + 1).text != "=" ||
            i + 3 == significant.size() || at(i + 3).text != "(")
            continue;
        const auto conversion = at(i + 2).text;
        if (conversion != "strtod" && conversion != "strtold" && conversion != "strtof" &&
            conversion != "atof" && conversion != "aurocks_strtod" && conversion != "aurocks_strtold" &&
            conversion != "aurocks_strtof" && conversion != "aurocks_atof")
            continue;
        unsigned depth = 1;
        for (std::size_t end = i + 4; end < significant.size(); ++end) {
            if (at(end).text == "(")
                ++depth;
            if (at(end).text == ")" && !--depth) {
                // Only a direct assignment of the call's result is boxed.
                // Do not silently change C arithmetic surrounding a call.
                const auto next = end + 1 == significant.size() ? std::string_view{} : at(end + 1).text;
                if (next.empty() || next == ";" || next == ")" || next == ",") {
                    at(i + 2).before += "aurocks_box_double(";
                    if (!conversion.starts_with("aurocks_"))
                        at(i + 2).replacement = "aurocks_" + std::string(conversion);
                    at(end).after += ')';
                }
                break;
            }
        }
    }
    for (const auto& tok : tokens) {
        action.code += tok.before;
        action.code += tok.replacement.empty() ? tok.text : std::string_view(tok.replacement);
        action.code += tok.after;
    }
    return action;
}

void emitActions(std::ostream& out, const Grammar& grammar, const std::vector<Action>& actions) {
    if (std::any_of(actions.begin(), actions.end(), [](const auto& action) { return action.strdup; })) {
        out << "static char *my_strdup(const char *value) {\n"
               "    const size_t n = strlen(value) + 1;\n"
               "    char *copy = (char *)malloc(n);\n"
               "    if (copy) memcpy(copy, value, n);\n"
               "    return copy;\n"
               "}\n\n";
    }
    out << "static void *aurocks_action(size_t rule, void **values, char *token) {\n"
           "    void *result = NULL;\n"
           "    (void)values;\n"
           "    (void)token;\n"
           "    switch (rule) {\n";
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto& action = actions[i];
        const auto& rule = grammar.rules[i];
        const bool leftRecursive = !rule.rhs.empty() && rule.rhs[0].kind == SymbolKind::nonterminal &&
                                   rule.rhs[0].text == rule.lhs;
        if (action.code.empty() && !leftRecursive)
            continue;
        out << "    case " << i << ": {\n";
        for (const auto index : action.usedValues) {
            out << "        " << (terminal(rule.rhs[index - 1].kind) ? "char" : "void")
                << " *v" << index << " = values[" << index - 1 << "];\n";
        }
        if (leftRecursive)
            out << "        result = values[0];\n";
        out << "        {" << action.code << "\n        }\n        break;\n    }\n";
    }
    out << "    default: break;\n    }\n    return result;\n}\n\n";
}

void emitTerminalUse(std::ostream& out, const std::vector<Action>& actions) {
    out << "static unsigned aurocks_terminal_use(size_t rule, size_t position) {\n"
           "    (void)position;\n"
           "    switch (rule) {\n";
    for (std::size_t rule = 0; rule < actions.size(); ++rule) {
        const auto& use = actions[rule].terminalUse;
        if (std::all_of(use.begin(), use.end(), [](auto mask) { return !mask; }))
            continue;
        out << "    case " << rule << ":\n";
        for (std::size_t position = 0; position < use.size(); ++position) {
            if (use[position])
                out << "        if (position == " << position << ") return " << use[position] << ";\n";
        }
        out << "        break;\n";
    }
    out << "    default: break;\n    }\n    return 0;\n}\n\n";
}

void validateDirectives(const Grammar& grammar, Diagnostics& diagnostics) {
    const auto ignored = [&](const auto& d) {
        diagnostics.push_back({Severity::warning, d.location, "unsupported-directive",
                               "%" + d.name + " is retained as metadata and has no parser behavior"});
    };
    static const std::set<std::string_view> declarations{
        "start", "skip", "backend", "algorithm", "algo", "disambiguator", "expect", "rewriter", "fault",
    };
    for (const auto& d : grammar.directives) {
        if (!declarations.count(d.name))
            ignored(d);
    }
    for (const auto& rule : grammar.rules) {
        bool hasPreference = false, hasPrecedence = false;
        for (const auto& d : rule.directives) {
            if (d.name == "prefer" || d.name == "avoid") {
                if (hasPreference)
                    error(diagnostics, d.location, "duplicate-preference",
                          "a production may have only one %prefer or %avoid");
                if (!d.value.empty())
                    error(diagnostics, d.location, "invalid-preference", "%" + d.name + " takes no argument");
                hasPreference = true;
            } else if (d.name == "dprec" || d.name == "priority") {
                int value;
                if (hasPrecedence)
                    error(diagnostics, d.location, "duplicate-precedence",
                          "a production may have only one %dprec or %priority");
                if (!integer(d.value, value))
                    error(diagnostics, d.location, "invalid-precedence", "%" + d.name + " requires a signed integer");
                hasPrecedence = true;
            } else {
                ignored(d);
            }
        }
    }
}

using Nonterminals = std::unordered_map<std::string, std::size_t>;

Nonterminals collectNames(const Grammar& grammar) {
    Nonterminals names;
    for (const auto& rule : grammar.rules)
        names.try_emplace(rule.lhs, names.size());
    return names;
}

// A unit/same-span recursion would create a cyclic forest with unbounded
// derivations of one string. Consuming left (and right) recursion is fine.
void validateForest(const Grammar& grammar, const Nonterminals& names, Diagnostics& diagnostics) {
    std::vector<bool> nullable(names.size());
    const auto isNullable = [&](const Symbol& s) {
        return s.kind == SymbolKind::empty ||
               (s.kind == SymbolKind::nonterminal && nullable[names.at(s.text)]);
    };
    bool changed;
    do {
        changed = false;
        for (const auto& rule : grammar.rules) {
            const auto lhs = names.at(rule.lhs);
            if (!nullable[lhs] && std::all_of(rule.rhs.begin(), rule.rhs.end(), isNullable))
                changed = nullable[lhs] = true;
        }
    } while (changed);

    std::vector<std::size_t> incoming(names.size());
    std::vector<std::vector<std::size_t>> outgoing(names.size());
    for (const auto& rule : grammar.rules) {
        const auto lhs = names.at(rule.lhs);
        const auto nonempty = std::count_if(rule.rhs.begin(), rule.rhs.end(),
                                            [&](const auto& s) { return !isNullable(s); });
        for (const auto& s : rule.rhs) {
            if (s.kind == SymbolKind::nonterminal && nonempty <= (isNullable(s) ? 0 : 1)) {
                const auto rhs = names.at(s.text);
                outgoing[lhs].push_back(rhs);
                ++incoming[rhs];
            }
        }
    }
    std::vector<std::size_t> todo;
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        if (!incoming[i])
            todo.push_back(i);
    }
    for (std::size_t i = 0; i < todo.size(); ++i) {
        for (const auto target : outgoing[todo[i]]) {
            if (!--incoming[target])
                todo.push_back(target);
        }
    }
    if (todo.size() != names.size()) {
        const auto cycle = std::find_if(grammar.rules.begin(), grammar.rules.end(), [&](const auto& rule) {
            return incoming[names.at(rule.lhs)] != 0;
        });
        error(diagnostics, cycle->location, "cyclic-forest",
              "grammar can recurse without consuming input; cyclic parse forests are not supported");
    }
}

void quote(std::ostream& out, std::string_view text) {
    out << '"';
    for (const unsigned char c : text) {
        // Octal escapes have fixed length and do not combine with the next
        // byte; escaping '?' also avoids accidental C99 trigraphs.
        if (c >= 32 && c <= 126 && c != '\\' && c != '"' && c != '?')
            out << static_cast<char>(c);
        else
            out << '\\' << (c >> 6) << ((c >> 3) & 7) << (c & 7);
    }
    out << '"';
}

void emitPortable(std::ostream& out, const Grammar& grammar, const Analysis& analysis,
                  const std::vector<Action>& actions, std::string_view entrypoint,
                  Diagnostics& diagnostics) {
    const auto names = collectNames(grammar);
    validateForest(grammar, names, diagnostics);
    portable::RegexProgram regex;
    std::unordered_map<std::string, int> patterns;
    auto addRegex = [&](const std::string& pattern, Location location) {
        const auto found = patterns.find(pattern);
        if (found != patterns.end())
            return found->second;
        try {
            const auto start = regex.add(pattern);
            patterns.emplace(pattern, start);
            return start;
        } catch (const std::runtime_error& exception) {
            error(diagnostics, location, "invalid-regex", exception.what());
            return 0;
        }
    };
    int skip = 0;
    if (grammar.skip) {
        const auto directive = std::find_if(grammar.directives.begin(), grammar.directives.end(),
                                             [](const auto& d) { return d.name == "skip"; });
        skip = addRegex(*grammar.skip, directive == grammar.directives.end() ? Location{} : directive->location);
    }
    std::vector<std::vector<std::size_t>> productions(names.size());
    std::size_t maxRhs = 1, offset = 0;
    out << "typedef struct { unsigned kind; size_t id, length; const char *literal; unsigned use; } aurocks_symbol;\n"
           "typedef struct { size_t lhs, offset, length; int precedence, preference; } aurocks_rule;\n"
           "typedef struct { unsigned kind; size_t next, other; } aurocks_rx_state;\n\n"
           "static const aurocks_symbol aurocks_rhs[] = {\n";
    for (std::size_t n = 0; n < grammar.rules.size(); ++n) {
        const auto& rule = grammar.rules[n];
        productions[names.at(rule.lhs)].push_back(n);
        maxRhs = std::max(maxRhs, rule.rhs.size());
        for (std::size_t i = 0; i < rule.rhs.size(); ++i) {
            const auto& s = rule.rhs[i];
            switch (s.kind) {
            case SymbolKind::nonterminal: out << "    {0, " << names.at(s.text) << ", 0, NULL, 0},\n"; break;
            case SymbolKind::literal:
                out << "    {1, 0, " << s.text.size() << ", ";
                quote(out, s.text);
                out << ", " << actions[n].terminalUse[i] << "},\n";
                break;
            case SymbolKind::regex:
                out << "    {2, " << addRegex(s.text, s.location) << ", 0, NULL, " <<
                    actions[n].terminalUse[i] << "},\n";
                break;
            case SymbolKind::empty: out << "    {3, 0, 0, NULL, 0},\n"; break;
            }
            ++offset;
        }
    }
    if (!offset)
        out << "    {0, 0, 0, NULL, 0},\n";
    out << "};\n\nstatic const aurocks_rule aurocks_rules[] = {\n";
    offset = 0;
    const bool preferFirst = analysis.disambiguator == "prefer";
    for (const auto& rule : grammar.rules) {
        int preference = 0, precedence = 0;
        for (const auto& d : rule.directives) {
            if (d.name == "prefer")
                preference = 1;
            else if (d.name == "avoid")
                preference = -1;
            else if (d.name == "dprec" || d.name == "priority")
                integer(d.value, precedence);
        }
        out << "    {" << names.at(rule.lhs) << ", " << offset << ", " << rule.rhs.size()
            << ", " << (preferFirst ? preference : precedence)
            << ", " << (preferFirst ? precedence : preference) << "},\n";
        offset += rule.rhs.size();
    }
    out << "};\n\nstatic const size_t aurocks_productions[] = {";
    offset = 0;
    for (const auto& byLhs : productions) {
        out << offset << ", ";
        offset += byLhs.size();
    }
    out << offset << "};\nstatic const size_t aurocks_production_ids[] = {";
    for (const auto& byLhs : productions) {
        for (const auto id : byLhs)
            out << id << ", ";
    }
    out << "};\n\nstatic const unsigned char aurocks_sets[][32] = {\n";
    if (regex.sets.empty())
        out << "    {0},\n";
    for (const auto& set : regex.sets) {
        out << "    {";
        for (std::size_t i = 0; i < 256; i += 8) {
            unsigned byte = 0;
            for (unsigned j = 0; j < 8; ++j) {
                if (set.test(i + j))
                    byte |= 1u << j;
            }
            out << byte << ", ";
        }
        out << "},\n";
    }
    out << "};\n\nstatic const aurocks_rx_state aurocks_states[] = {\n";
    for (const auto& s : regex.states)
        out << "    {" << static_cast<unsigned>(s.kind) << ", " << s.next << ", " << s.other << "},\n";
    out << "};\n\n"
        << "#define AUROCKS_RX_STATES " << regex.states.size() << '\n'
        << "#define AUROCKS_RULES " << grammar.rules.size() << '\n'
        << "#define AUROCKS_MAX_RHS " << maxRhs << '\n'
        << "#define AUROCKS_INPUT_LIMIT 1048576UL\n"
           "#define AUROCKS_ITEM_LIMIT 1048576UL\n"
           "#define AUROCKS_EDGE_LIMIT 4194304UL\n"
           "#define AUROCKS_WORK_LIMIT 67108864UL\n"
        << "static const size_t aurocks_start = " << names.at(grammar.start) << ";\n"
        << "static const size_t aurocks_skip_rx = " << skip << ";\n"
        << "static const int aurocks_has_skip = " << grammar.skip.has_value() << ";\n"
        << "static const int aurocks_ambiguity_error = " << (analysis.disambiguator == "error") << ";\n"
        << portable::Runtime;

    const auto defaultName = "parse_" + grammar.start;
    out << R"aurocks(
/* Last error is per-thread where TLS is available. On C99 implementations
 * without TLS, callers must serialize use of the compatibility entrypoints. */
static AUROCKS_THREAD_LOCAL const char *aurocks_error_position;
)aurocks"
        << "void *" << defaultName << "(const char *input) {\n"
           "    const char *error;\n"
           "    void *value = aurocks_parse_input(input, &error);\n"
           "    aurocks_error_position = error;\n"
           "    return value;\n"
           "}\n"
        << "const char *" << defaultName << "_error(void) { return aurocks_error_position; }\n"
        << "void *yyparse(const char *input) { return " << defaultName << "(input); }\n"
        << "const char *yyparse_error(void) { return aurocks_error_position; }\n";
    if (!entrypoint.empty() && entrypoint != defaultName && entrypoint != "yyparse") {
        out << "void *" << entrypoint << "(const char *input) { return " << defaultName << "(input); }\n"
            << "const char *" << entrypoint << "_error(void) { return aurocks_error_position; }\n";
    }
}

bool checkEntrypoint(const Grammar& grammar, std::string_view entrypoint, Diagnostics& diagnostics) {
    if (entrypoint.empty() || (validEntrypoint(entrypoint) &&
                               entrypoint != "parse_" + grammar.start + "_error"))
        return true;
    error(diagnostics, {}, "invalid-entrypoint", "entry point must be a non-reserved C identifier");
    return false;
}

} // namespace

void validateEmission(const Grammar& grammar, const Analysis& analysis, std::string_view entrypoint,
                      Diagnostics& diagnostics) {
    checkEntrypoint(grammar, entrypoint, diagnostics);
    const bool legacySList = grammar.prologue.find("SList") != std::string::npos;
    for (const auto& rule : grammar.rules)
        transform(rule, legacySList, diagnostics);
    validateDirectives(grammar, diagnostics);
    if (analysis.backend != "libglr")
        validateForest(grammar, collectNames(grammar), diagnostics);
}

std::string emit(const Grammar& grammar, const Analysis& analysis, std::string_view entrypoint,
                 Diagnostics& diagnostics) {
    if (!checkEntrypoint(grammar, entrypoint, diagnostics))
        return {};
    std::vector<Action> actions;
    actions.reserve(grammar.rules.size());
    const bool legacySList = grammar.prologue.find("SList") != std::string::npos;
    for (const auto& rule : grammar.rules)
        actions.push_back(transform(rule, legacySList, diagnostics));
    validateDirectives(grammar, diagnostics);
    if (std::any_of(diagnostics.begin(), diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.severity == Severity::error; }))
        return {};

    std::ostringstream out;
    out << LegacyRuntimeHelpers << '\n' << grammar.prologue << '\n';
    emitActions(out, grammar, actions);
    if (analysis.backend == "libglr") {
        emitTerminalUse(out, actions);
        emitLinkedParser(out, grammar, analysis, entrypoint, diagnostics);
    } else {
        emitPortable(out, grammar, analysis, actions, entrypoint, diagnostics);
    }
    out << grammar.epilogue;
    if (std::any_of(diagnostics.begin(), diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.severity == Severity::error; }))
        return {};
    return out.str();
}

} // namespace aurocks::detail
