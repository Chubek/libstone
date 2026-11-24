#include "LinkedEmitter.hpp"

#include <charconv>
#include <map>
#include <ostream>
#include <utility>

namespace aurocks::detail {
namespace {

void quote(std::ostream& out, std::string_view value) {
    out << '"';
    for (unsigned char ch : value) {
        if (ch >= 32 && ch <= 126 && ch != '"' && ch != '\\' && ch != '?')
            out << static_cast<char>(ch);
        else
            out << '\\' << ((ch >> 6) & 7) << ((ch >> 3) & 7) << (ch & 7);
    }
    out << '"';
}

} // namespace

void emitLinkedParser(std::ostream& out, const Grammar& grammar, const Analysis& analysis,
                      std::string_view entrypoint, std::vector<Diagnostic>& diagnostics) {
    using Key = std::pair<SymbolKind, std::string>;
    std::map<Key, std::size_t> lookup;
    std::vector<Key> symbols;
    auto intern = [&](SymbolKind kind, const std::string& text) {
        auto [it, inserted] = lookup.try_emplace(Key{kind, text}, symbols.size());
        if (inserted) symbols.push_back(it->first);
        return it->second;
    };
    for (const auto& rule : grammar.rules) {
        intern(SymbolKind::nonterminal, rule.lhs);
        for (const auto& symbol : rule.rhs)
            if (symbol.kind != SymbolKind::empty) intern(symbol.kind, symbol.text);
    }
    out << "\n#include <aurocks/Runtime.h>\n"
           "static const aurocks_symbol_spec aurocks_symbols[] = {\n";
    for (const auto& [kind, text] : symbols) {
        out << "  {" << (kind == SymbolKind::nonterminal ? "AUROCKS_NONTERMINAL"
                         : kind == SymbolKind::literal ? "AUROCKS_LITERAL" : "AUROCKS_REGEX")
            << ", ";
        quote(out, text);
        out << ", " << text.size() << "},\n";
    }
    out << "};\n";
    for (std::size_t r = 0; r < grammar.rules.size(); ++r) {
        const auto& rule = grammar.rules[r];
        if (rule.rhs.empty()) continue;
        out << "static const int aurocks_rhs_" << r << "[] = {";
        for (const auto& symbol : rule.rhs) {
            if (symbol.kind == SymbolKind::empty)
                out << "-1,";
            else
                out << lookup.at({symbol.kind, symbol.text}) << ',';
        }
        out << "};\n";
    }
    out << "static const aurocks_rule_spec aurocks_rules[] = {\n";
    for (std::size_t r = 0; r < grammar.rules.size(); ++r) {
        const auto& rule = grammar.rules[r];
        long priority = 0;
        int preference = 0;
        for (const auto& directive : rule.directives) {
            if (directive.name == "dprec" || directive.name == "priority") {
                auto text = std::string_view(directive.value);
                if (!text.empty() && text.front() == '+') text.remove_prefix(1);
                auto [ptr, status] = std::from_chars(text.data(), text.data() + text.size(), priority);
                if (status != std::errc{} || ptr != text.data() + text.size()) {
                    diagnostics.push_back({Severity::error, directive.location, "invalid-priority",
                                           "expected an integer after %" + directive.name});
                }
            } else if (directive.name == "prefer") {
                preference = 1;
            } else if (directive.name == "avoid") {
                preference = -1;
            }
        }
        out << "  {" << lookup.at({SymbolKind::nonterminal, rule.lhs}) << ", ";
        if (rule.rhs.empty()) out << "NULL";
        else out << "aurocks_rhs_" << r;
        // Avoid the implementation-defined type of the most negative literal.
        if (priority < 0)
            out << ", " << rule.rhs.size() << ", (-" << -(priority + 1) << "L - 1L)";
        else
            out << ", " << rule.rhs.size() << ", " << priority << 'L';
        out << ", " << preference << "},\n";
    }
    out << "};\nstatic const aurocks_parser_spec aurocks_parser = {\n"
        << "  aurocks_symbols, " << symbols.size() << ", aurocks_rules, " << grammar.rules.size()
        << ", " << lookup.at({SymbolKind::nonterminal, grammar.start}) << ", ";
    if (grammar.skip) quote(out, *grammar.skip);
    else out << "NULL";
    out << ", " << (analysis.disambiguator == "error" ? "AUROCKS_REJECT_AMBIGUOUS"
                    : analysis.disambiguator == "prefer" ? "AUROCKS_PREFER"
                    : analysis.disambiguator == "priority" ? "AUROCKS_PRIORITY" : "AUROCKS_FIRST")
        << ", aurocks_action, aurocks_terminal_use\n};\n";
    const std::string canonical = "parse_" + grammar.start;
    const auto entry = entrypoint.empty() ? std::string_view(canonical) : entrypoint;
    out << "static AUROCKS_THREAD_LOCAL const char *aurocks_error_position;\n"
        << "void *" << entry << "(const char *input) { "
        << "return aurocks_parse_glr_ex(&aurocks_parser, input, &aurocks_error_position); }\n"
        << "const char *" << entry << "_error(void) { return aurocks_error_position; }\n";
    if (entry != canonical)
        out << "void *" << canonical << "(const char *input) { return " << entry << "(input); }\n"
            << "const char *" << canonical << "_error(void) { return aurocks_error_position; }\n";
    if (entry != "yyparse")
        out << "void *yyparse(const char *input) { return " << entry << "(input); }\n"
            << "const char *yyparse_error(void) { return aurocks_error_position; }\n";
}

} // namespace aurocks::detail
