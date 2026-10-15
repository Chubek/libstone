#include "Internal.hpp"
#include <charconv>

namespace gateml {
namespace {
struct Token { std::string text, kind; Location loc; };
// Parenthesized infix operators are curried values. Prefix negation remains
// ordinary syntax; `(not)` names the unary boolean function.
const std::set<std::string> operators = {
    "+", "-", "*", "/", "mod", "=", "<>", "<", ">", "<=", ">=", "&&", "||", "^"
};
const std::set<std::string> reserved = {
    "let", "rec", "in", "fun", "if", "then", "else", "match", "with",
    "true", "false", "mod", "not", "module", "struct", "end", "open", "import"
};
class Parser {
    std::vector<Token> tokens;
    std::size_t pos = 0, depth = 0;
    std::size_t module_depth = 0;
    std::map<std::string, std::set<std::string>> modules;
    const Options& options;
    const Token& peek() const { return tokens[pos]; }
    bool at(std::string_view s) const { return peek().text == s; }
    Token take() { auto t = peek(); if (pos + 1 < tokens.size()) ++pos; return t; }
    bool accept(std::string_view s) { if (!at(s)) return false; take(); return true; }
    Token expect(std::string_view s) {
        if (!at(s)) throw Error(peek().loc, "expected '" + std::string(s) + "', found '" + peek().text + "'");
        return take();
    }
    bool identifier() const { return peek().kind == "@Name" && !reserved.contains(peek().text); }
    Token name() {
        if (!identifier()) throw Error(peek().loc, "expected identifier");
        return take();
    }
    Tree node(Expr::Kind kind, Location loc, std::vector<Tree> children = {}, std::string text = {}) {
        auto e = std::make_shared<Expr>(); e->kind = kind; e->loc = std::move(loc);
        for (const auto& c : children) e->height = std::max(e->height, c->height + 1);
        if (e->height > options.max_depth) throw Error(e->loc, "expression depth limit exceeded");
        e->children = std::move(children); e->text = std::move(text); return e;
    }
    struct Guard {
        Parser& p;
        explicit Guard(Parser& p) : p(p) {
            if (++p.depth > p.options.max_depth) throw Error(p.peek().loc, "syntax nesting limit exceeded");
        }
        ~Guard() { --p.depth; }
    };
    void rename(const Tree& e, std::map<std::string, std::string> names) {
        if (e->kind == Expr::Variable) {
            auto i = names.find(e->text); if (i != names.end()) e->text = i->second;
        } else if (e->kind == Expr::Lambda) {
            names.erase(e->text); rename(e->children[0], names);
        } else if (e->kind == Expr::Let) {
            auto rhs = names; if (e->recursive) rhs.erase(e->text);
            rename(e->children[0], rhs); names.erase(e->text); rename(e->children[1], names);
        } else if (e->kind == Expr::Match) {
            rename(e->children[0], names);
            for (std::size_t n = 0; n < e->patterns.size(); ++n) {
                auto branch = names;
                std::function<void(const Pattern&)> erase = [&](const Pattern& p) {
                    if (p.kind == Pattern::Bind) branch.erase(p.text);
                    for (const auto& c : p.children) erase(c);
                };
                erase(e->patterns[n]); rename(e->children[n + 1], branch);
            }
        } else for (const auto& c : e->children) rename(c, names);
    }
    std::int64_t integer(const Token& t, bool negative = false) {
        std::string text = negative ? "-" + t.text : t.text;
        std::int64_t n = 0;
        auto r = std::from_chars(text.data(), text.data() + text.size(), n);
        if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
            throw Error(t.loc, "integer literal is outside signed 64-bit range");
        return n;
    }
    Tree binding(bool top) {
        const auto loc = expect("let").loc;
        bool rec = accept("rec");
        auto id = name();
        std::vector<Token> params;
        while (identifier()) params.push_back(name());
        expect("="); auto rhs = expression();
        for (auto p = params.rbegin(); p != params.rend(); ++p)
            rhs = node(Expr::Lambda, p->loc, {rhs}, p->text);
        if (rec && rhs->kind != Expr::Lambda) throw Error(loc, "let rec requires a function");
        Tree body;
        if (accept("in")) body = expression();
        else if (top) { expect(";;"); body = program(); }
        else throw Error(peek().loc, "expected 'in' in local binding");
        auto e = node(Expr::Let, loc, {rhs, body}, id.text); e->recursive = rec; return e;
    }
    static int precedence(const std::string& s) {
        if (s == ";") return 1;
        if (s == "||") return 2;
        if (s == "&&") return 3;
        if (s == "=" || s == "<>" || s == "<" || s == ">" || s == "<=" || s == ">=") return 4;
        if (s == "::") return 5;
        if (s == "^") return 6;
        if (s == "+" || s == "-") return 7;
        if (s == "*" || s == "/" || s == "mod") return 8;
        return 0;
    }
    bool atom_start() const {
        return peek().kind == "@Int" || peek().kind == "@String" || identifier() ||
               at("(") || at("[") || at("true") || at("false");
    }
    Tree expression(int minimum = 1) {
        Guard guard(*this);
        auto left = prefix();
        for (;;) {
            if (atom_start() && minimum <= 10) {
                auto loc = peek().loc;
                left = node(Expr::Apply, loc, {left, expression(11)}); continue;
            }
            int p = precedence(peek().text);
            if (p < minimum || p == 0) break;
            auto op = take();
            auto right = expression(p + (op.text == "::" || op.text == ";" ? 0 : 1));
            left = node(op.text == "::" ? Expr::Cons : op.text == ";" ? Expr::Sequence : Expr::Binary,
                        op.loc, {left, right}, op.text);
        }
        return left;
    }
    Tree prefix() {
        auto t = peek();
        if (at("let")) return binding(false);
        if (accept("if")) {
            auto condition = expression(); expect("then"); auto yes = expression();
            expect("else"); return node(Expr::If, t.loc, {condition, yes, expression()});
        }
        if (accept("fun")) {
            std::vector<Token> params{name()};
            while (identifier()) params.push_back(name());
            expect("->"); auto body = expression();
            for (auto p = params.rbegin(); p != params.rend(); ++p)
                body = node(Expr::Lambda, p->loc, {body}, p->text);
            return body;
        }
        if (accept("match")) {
            auto e = node(Expr::Match, t.loc, {expression()}); expect("with"); accept("|");
            do {
                e->patterns.push_back(pattern()); expect("->"); e->children.push_back(expression());
            } while (accept("|"));
            return e;
        }
        if (accept("-") || accept("not")) {
            if (t.text == "-" && peek().kind == "@Int") {
                auto e = node(Expr::Integer, t.loc); e->number = integer(take(), true); return e;
            }
            return node(Expr::Unary, t.loc, {expression(9)}, t.text);
        }
        if (accept("(")) {
            if (accept(")")) return node(Expr::Unit, t.loc);
            // Match text rather than token kind: `mod` and `not` are names.
            // Look ahead without consuming so `(-1)` and `(not false)` retain
            // their ordinary prefix-expression meaning.
            if ((operators.contains(peek().text) || at("not")) &&
                pos + 1 < tokens.size() && tokens[pos + 1].text == ")") {
                auto op = take(); expect(")");
                return node(Expr::Variable, t.loc, {}, op.text == "not" ? "#not" : op.text);
            }
            auto a = expression();
            if (accept(",")) a = node(Expr::Pair, t.loc, {a, expression()});
            expect(")"); return a;
        }
        if (accept("[")) {
            std::vector<Tree> items;
            if (!at("]")) do { items.push_back(expression(2)); } while (accept(";"));
            expect("]"); auto e = node(Expr::Nil, t.loc);
            for (auto i = items.rbegin(); i != items.rend(); ++i) e = node(Expr::Cons, t.loc, {*i, e});
            return e;
        }
        if (t.kind == "@Int") {
            take(); auto e = node(Expr::Integer, t.loc); e->number = integer(t); return e;
        }
        if (t.kind == "@String") {
            take(); std::string s;
            for (std::size_t i = 1; i + 1 < t.text.size(); ++i) {
                char c = t.text[i];
                if (c == '\\') {
                    c = t.text[++i];
                    switch (c) {
                    case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                    case '\\': case '"': break;
                    default: throw Error(t.loc, "unsupported string escape");
                    }
                }
                s.push_back(c);
            }
            return node(Expr::String, t.loc, {}, std::move(s));
        }
        if (accept("true") || accept("false")) {
            auto e = node(Expr::Boolean, t.loc); e->number = t.text == "true"; return e;
        }
        if (identifier()) return node(Expr::Variable, t.loc, {}, take().text);
        throw Error(t.loc, "expected expression, found '" + t.text + "'");
    }
    Pattern pattern(bool cons = true) {
        Guard guard(*this); auto t = take(); Pattern p; p.loc = t.loc;
        if (t.text == "_") p.kind = Pattern::Wild;
        else if (t.kind == "@Int" || t.text == "-") {
            p.kind = Pattern::Integer; p.number = integer(t.text == "-" ? take() : t, t.text == "-");
        } else if (t.text == "true" || t.text == "false") { p.kind = Pattern::Boolean; p.number = t.text == "true"; }
        else if (t.text == "[") { expect("]"); p.kind = Pattern::Nil; }
        else if (t.text == "(") {
            if (accept(")")) p.kind = Pattern::Unit;
            else {
                p = pattern();
                if (accept(",")) { Pattern q; q.kind = Pattern::Pair; q.loc = t.loc; q.children = {p, pattern()}; p = q; }
                expect(")");
            }
        } else if (t.text == "Some" || t.text == "Ok" || t.text == "Error") {
            p.kind = t.text == "Some" ? Pattern::Some : t.text == "Ok" ? Pattern::Ok : Pattern::Error;
            p.children.push_back(pattern(false));
        } else if (t.text == "None") p.kind = Pattern::None;
        else if (t.kind == "@Name" && !reserved.contains(t.text)) { p.kind = Pattern::Bind; p.text = t.text; }
        else throw Error(t.loc, "invalid pattern");
        if (cons && accept("::")) {
            Pattern q; q.kind = Pattern::Cons; q.loc = t.loc; q.children = {p, pattern()}; return q;
        }
        return p;
    }
public:
    Parser(std::vector<Token> tokens, const Options& options) : tokens(std::move(tokens)), options(options) {
        auto add = [&](const std::string& name) {
            auto dot = name.find('.');
            if (dot != std::string::npos) modules[name.substr(0, dot)].insert(name.substr(dot + 1));
        };
        unsigned next = 1;
        for (const auto& [name, scheme] : builtin_types(next)) { (void)scheme; add(name); }
        if (options.standard_library) {
            static const auto exports = [] {
                std::vector<std::string> names;
                std::string source(standard_library());
                const std::regex declaration(R"(\blet\s+(?:rec\s+)?([A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*))");
                for (std::sregex_iterator i(source.begin(), source.end(), declaration), end; i != end; ++i)
                    names.push_back((*i)[1].str());
                return names;
            }();
            for (const auto& name : exports) add(name);
        }
    }
    Tree program() {
        Guard guard(*this);
        if (accept("module")) {
            auto module = name(); expect("="); expect("struct"); ++module_depth;
            auto definitions = program(); --module_depth; expect("end"); expect(";;");
            if (definitions->kind != Expr::Let && definitions->kind != Expr::Unit)
                throw Error(module.loc, "module bodies contain only top-level bindings");
            auto current = definitions;
            modules.try_emplace(module.text);
            std::map<std::string, std::string> names;
            while (current->kind == Expr::Let) {
                auto original = current->text, qualified = module.text + "." + original;
                auto rhs = names; if (current->recursive) rhs[original] = qualified;
                rename(current->children[0], rhs); current->text = qualified; names[original] = qualified;
                modules[module.text].insert(original);
                if (current->children[1]->kind == Expr::Unit) break;
                current = current->children[1];
            }
            if (current->kind != Expr::Unit && (current->kind != Expr::Let || current->children[1]->kind != Expr::Unit))
                throw Error(module.loc, "module bodies contain only top-level bindings");
            auto rest = program();
            if (definitions->kind == Expr::Unit) return rest;
            current->children[1] = rest; return definitions;
        }
        if (accept("open")) {
            auto module = name(); expect(";;");
            auto i = modules.find(module.text);
            if (i == modules.end()) throw Error(module.loc, "unknown module: " + module.text);
            auto exports = i->second; auto rest = program(); std::map<std::string, std::string> names;
            for (const auto& n : exports) names[n] = module.text + "." + n;
            rename(rest, names); return rest;
        }
        if (at("let")) return binding(true);
        if (module_depth && at("end")) return node(Expr::Unit, peek().loc);
        if (at("<eof>")) return node(Expr::Unit, peek().loc);
        if (module_depth) throw Error(peek().loc, "module bodies contain only top-level bindings");
        auto e = expression(); accept(";;");
        if (!at("<eof>")) throw Error(peek().loc, "expected end of program (top-level bindings end with ';;')");
        return e;
    }
    Tree finish() {
        auto e = program(); accept(";;");
        if (!at("<eof>")) throw Error(peek().loc, "unexpected trailing input");
        return e;
    }
};
}
Tree parse(std::string_view source, const std::string& file, const Options& options) {
    if (source.size() > options.max_source_bytes) throw Error({file}, "source size limit exceeded");
    auto clean = preprocess(source, file);
    parsetk::Registry grammar;
    using K = parsetk::Symbol::Kind;
    grammar.add_symbol({K::Lexeme, "@Int", parsetk::PatternSpec::regex("[0-9]+"), {}});
    grammar.add_symbol({K::Lexeme, "@String", parsetk::PatternSpec::regex(R"re("([^"\\\r\n]|\\.)*")re"), {}});
    grammar.add_symbol({K::Lexeme, "@Name", parsetk::PatternSpec::regex("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*"), {}});
    grammar.add_symbol({K::Lexeme, "@Symbol", parsetk::PatternSpec::regex(R"(::|;;|->|<=|>=|<>|&&|\|\||[()\[\],;=+*/%<>^|:\-])"), {}});
    grammar.add_symbol({K::Production, "#Tokens", {}, "(@Int | @String | @Name | @Symbol)*"});
    metachain::Options config;
    config.parser.max_input_bytes = options.max_source_bytes;
    config.parser.max_events = options.max_values;
    config.parser.max_operations = options.max_values > SIZE_MAX / 32 ? SIZE_MAX : options.max_values * 32;
    auto parsed = metachain::Toolchain(config).parse(clean, grammar, "#Tokens");
    if (!parsed) {
        const auto& d = parsed.diagnostics.front();
        throw Error({file, d.line, d.column}, "lexical error: " + d.message);
    }
    std::vector<Token> tokens;
    for (const auto& e : *parsed) if (e.kind == parsetk::EventKind::Token)
        tokens.push_back({e.text, e.name, {file, e.span.line, e.span.column}});
    Location end{file};
    for (char c : source) { if (c == '\n') { ++end.line; end.column = 1; } else ++end.column; }
    tokens.push_back({"<eof>", {}, end});
    return Parser(std::move(tokens), options).finish();
}
} // namespace gateml
