#include "asdl/schema.hpp"
#include "lexer.hpp"

#include <deque>
#include <set>
#include <utility>

namespace asdl {
namespace {
using namespace grammar;

class Parser {
public:
    Parser(std::string_view input, std::string_view file) : lex(input) {
        model.file = file;
    }

    Schema run() {
        try {
            envelope();
            bool wrapped = matches("module") && !definition_op(peek(1));
            if (wrapped) {
                take();
                model.module = identifier("a module name").text;
                expect("{");
            }
            while (peek().kind != TokenKind::end && !matches("%%") &&
                   !(wrapped && matches("}"))) {
                if (matches(".")) directive();
                else definition();
            }
            if (wrapped) expect("}");
            if (matches("%%")) {
                auto end = take();
                if (!delimited)
                    fail(end, "epilogue requires an initial %% before definitions");
                model.epilogue = fragment(lex.span(lex.pos, lex.input.size()));
            } else if (peek().kind != TokenKind::end) {
                fail(peek(), "unexpected text after definitions");
            }
            if (model.definitions.empty())
                Lexer::fail(lex.pos, "no ASDL definitions found");
            return std::move(model);
        } catch (const ScanError &error) {
            throw Diagnostic(span(error.at, error.at), error.message);
        }
    }

private:
    Lexer lex;
    Schema model;
    std::deque<Token> lookahead;
    bool delimited = false;
    std::size_t last_end = 0;
    std::set<std::string> metadata;

    SourcePos source(std::size_t at) const {
        return {at, lex.line(at), lex.column(at)};
    }
    SourceSpan span(std::size_t begin, std::size_t end) const {
        return {model.file, source(begin), source(end)};
    }
    Fragment fragment(Span input) const {
        auto where = span(input.at, input.at + input.text.size());
        return {std::move(input.text), std::move(where)};
    }
    [[noreturn]] static void fail(const Token &token, const std::string &message) {
        Lexer::fail(token.at, message);
    }
    const Token &peek(std::size_t i = 0) {
        while (lookahead.size() <= i) lookahead.push_back(lex.next());
        return lookahead[i];
    }
    bool matches(std::string_view text) {
        auto &token = peek();
        return token.kind != TokenKind::string && token.text == text;
    }
    Token take() {
        peek();
        auto token = std::move(lookahead.front());
        lookahead.pop_front();
        last_end = token.at + token.text.size();
        return token;
    }
    bool accept(std::string_view text) {
        if (!matches(text)) return false;
        take();
        return true;
    }
    Token expect(std::string_view text) {
        if (matches(text)) return take();
        auto &token = peek();
        fail(token, "expected '" + std::string(text) + "', found '" +
                        (token.kind == TokenKind::end ? "end of input" : token.text) + "'");
    }
    Token identifier(const char *description, char required_case = 0,
                     bool allow_underscore = false) {
        auto &token = peek();
        if (token.kind == TokenKind::identifier &&
            (required_case == 'l' ? lower(token.text[0]) :
             required_case == 'u' ? upper(token.text[0]) :
             allow_underscore || alpha(token.text[0])))
            return take();
        fail(token, std::string("expected ") + description);
    }
    static bool definition_op(const Token &token) {
        return token.kind == TokenKind::punct && (token.text == "=" || token.text == "::=");
    }
    std::string quoted(const char *description) {
        if (peek().kind != TokenKind::string) fail(peek(), std::string("expected ") + description);
        return take().text;
    }
    // Paths must be lexically contiguous: a subsequent directive is not a path
    // component even if the preceding definition omitted a legacy semicolon.
    Token qualified(Token first) {
        auto end = first.at + first.text.size();
        while (matches(".") && peek().at == end) {
            take();
            auto component = identifier("an identifier after '.'");
            if (component.at != end + 1)
                fail(component, "whitespace is not allowed inside a qualified name");
            end = component.at + component.text.size();
            first.text += "." + component.text;
        }
        return first;
    }
    Token type_name() {
        auto token = qualified(identifier("a lowercase field type or a qualified type"));
        auto leaf = token.text.find_last_of('.');
        if (!lower(token.text[leaf == std::string::npos ? 0 : leaf + 1]))
            fail(token, "expected a lowercase field type");
        return token;
    }

    void envelope() {
        lex.skip();
        bool preamble = false;
        if (lex.starts(lex.pos, "%{")) {
            auto opening = lex.pos;
            lex.pos += 2;
            model.prologue = fragment(lex.c_prologue(opening));
            preamble = true;
        }
        for (;;) {
            lex.skip();
            if (lex.ch(lex.pos) != '%' || lex.starts(lex.pos, "%%")) break;
            auto at = lex.pos++;
            auto end = lex.line_end(lex.pos);
            parse_option_line(at, end);
            lex.pos = end;
            preamble = true;
        }
        delimited = accept("%%");
        if (preamble && !delimited) fail(peek(), "expected initial %% after preamble");
    }

    std::vector<std::string> option_words(std::size_t at, std::size_t end) {
        std::vector<std::string> words;
        while (at < end) {
            while (at < end && space(lex.ch(at))) ++at;
            if (at == end || lex.ch(at) == '#' || lex.starts(at, "--") ||
                lex.starts(at, "//")) break;
            std::string word;
            while (at < end && !space(lex.ch(at))) {
                auto c = lex.ch(at++);
                if (c != '\'' && c != '"') {
                    word.push_back(c);
                    continue;
                }
                auto opening = at - 1;
                while (at < end && lex.ch(at) != c) {
                    if (lex.ch(at) == '\\' && ++at == end)
                        Lexer::fail(opening, "unfinished escape in option");
                    word.push_back(lex.ch(at++));
                }
                if (at == end) Lexer::fail(opening, "unterminated quoted option");
                ++at;
            }
            words.push_back(std::move(word));
        }
        return words;
    }
    void set_option(std::size_t at, std::string name, const std::string *value) {
        static const std::set<std::string> booleans{
            "enums", "line-directives", "header-only", "builtins"};
        static const std::set<std::string> strings{
            "prefix", "guard", "header", "source", "visitor", "listener",
            "include", "schema", "builtins-header", "omit-builtin"};
        if (name == "no-builtin") name = "omit-builtin";
        if (name.compare(0, 3, "no-") == 0) {
            name.erase(0, 3);
            if (!booleans.count(name)) Lexer::fail(at, "unknown boolean option '" + name + "'");
            if (value) Lexer::fail(at, "no-" + name + " does not take a value");
            model.options[name] = "0";
        } else if (booleans.count(name)) {
            if (!value || *value == "on" || *value == "yes" || *value == "true" || *value == "1")
                model.options[name] = "1";
            else if (*value == "off" || *value == "no" || *value == "false" || *value == "0")
                model.options[name] = "0";
            else
                Lexer::fail(at, "invalid boolean value '" + *value + "' for " + name);
        } else if (strings.count(name)) {
            if (!value || value->empty()) Lexer::fail(at, name + " requires a nonempty value");
            auto &setting = model.options[name];
            if (name == "omit-builtin" && !setting.empty()) setting += "," + *value;
            else setting = *value;
            if (name == "source") model.options["header-only"] = "0";
        } else {
            Lexer::fail(at, "unknown option '" + name + "'");
        }
    }
    void parse_option_line(std::size_t at, std::size_t end) {
        auto words = option_words(at + 1, end);
        if (words.empty()) Lexer::fail(at, "expected an option directive");
        if (words[0] == "option") {
            if (words.size() == 1) Lexer::fail(at, "%option requires at least one option");
            for (std::size_t i = 1; i < words.size(); ++i) {
                auto equals = words[i].find('=');
                if (equals == std::string::npos) set_option(at, words[i], nullptr);
                else {
                    auto value = words[i].substr(equals + 1);
                    set_option(at, words[i].substr(0, equals), &value);
                }
            }
        } else {
            if (words.size() > 2) Lexer::fail(at, "%" + words[0] + " takes at most one value");
            set_option(at, words[0], words.size() == 2 ? &words[1] : nullptr);
        }
    }

    Token type(unsigned depth = 0) {
        if (depth == 128) fail(peek(), "type nesting exceeds 128 levels");
        auto name = type_name();
        if (name.text != "list" && name.text != "array" && name.text != "map") return name;
        Generic generic;
        generic.kind = name.text;
        expect("[");
        if (name.text != "array" || peek().kind != TokenKind::number) {
            generic.args.push_back(type(depth + 1).text);
            if (name.text != "list") expect(",");
        }
        if (name.text == "map") generic.args.push_back(type(depth + 1).text);
        if (name.text == "array") {
            auto number = peek();
            auto first = number.text.find_first_not_of('0');
            if (number.kind != TokenKind::number || first == std::string::npos)
                fail(number, "array size must be a positive decimal integer");
            take();
            number.text.erase(0, first);
            if (number.text.size() > 18) fail(number, "array size is too large");
            generic.args.push_back(std::move(number.text));
        }
        auto close = expect("]");
        generic.span = span(name.at, close.at + 1);
        name.text += "[";
        for (std::size_t i = 0; i < generic.args.size(); ++i) {
            if (i) name.text += ",";
            name.text += generic.args[i];
        }
        name.text += "]";
        model.generics.emplace(name.text, std::move(generic));
        return name;
    }
    Token predicate(Token name, bool required) {
        if (accept("!")) name.text += "!";
        else if (required) fail(peek(), "expected '!' after executable refinement");
        return name;
    }
    std::vector<Field> fields(std::size_t &end) {
        expect("(");
        std::vector<Field> fields;
        do {
            auto name = type();
            Field field;
            field.type = std::move(name.text);
            auto last = last_end;
            if (accept("<")) {
                auto start = peek().at;
                if (accept(":")) {
                    field.refinement = ":" + identifier("a nominal refinement name").text;
                } else {
                    field.refinement = predicate(qualified(identifier("an executable refinement")),
                                                 true).text;
                    field.executable_refinement = true;
                }
                auto close = expect(">");
                last = close.at + 1;
                field.refinement_span = span(start, last - 1);
            }
            if (matches("?") || matches("*") || matches("+")) {
                auto qualifier = take();
                field.qualifier = qualifier.text[0];
                last = qualifier.at + 1;
            }
            if (peek().kind == TokenKind::identifier) {
                auto member = identifier("a field name");
                field.name = member.text;
                last = member.at + member.text.size();
            }
            field.span = span(name.at, last);
            fields.push_back(std::move(field));
        } while (accept(","));
        end = expect(")").at + 1;
        return fields;
    }
    void definition() {
        Definition definition;
        auto name = identifier("a lowercase type name", 'l');
        definition.name = name.text;
        definition.extended = accept("::=");
        if (!definition.extended) expect("=");
        std::size_t end = lex.pos;
        if (matches("(")) {
            definition.kind = DefinitionKind::product;
            definition.fields = fields(end);
        } else {
            definition.kind = DefinitionKind::sum;
            do {
                auto name = identifier("an uppercase constructor name", 'u');
                Constructor constructor;
                constructor.name = name.text;
                end = name.at + name.text.size();
                if (matches("(")) constructor.fields = fields(end);
                constructor.span = span(name.at, end);
                definition.constructors.push_back(std::move(constructor));
            } while (accept("|"));
            if (matches("attributes") && !definition_op(peek(1))) {
                take();
                definition.attributes = fields(end);
            }
        }
        if (definition.extended || matches(";")) end = expect(";").at + 1;
        definition.span = span(name.at, end);
        model.definitions.push_back(std::move(definition));
    }

    void directive() {
        auto dot = expect(".");
        auto keyword = identifier("an Extended ASDL directive");
        if (keyword.text == "import") return import(dot.at);
        if (keyword.text == "logistics") return logistics(dot.at);
        if (keyword.text != "language" && keyword.text != "summary" &&
            keyword.text != "version" && keyword.text != "unit" && keyword.text != "modular")
            fail(keyword, "unknown directive '." + keyword.text + "'");
        if (!metadata.insert(keyword.text).second)
            fail(keyword, "duplicate directive '." + keyword.text + "'");
        if (keyword.text == "language") {
            model.language = quoted("a quoted language name");
            if (model.language.empty()) fail(keyword, "language name must not be empty");
        } else if (keyword.text == "summary") {
            model.summary = quoted("a quoted summary");
        } else if (keyword.text == "version") {
            model.version = quoted("a quoted version");
        } else if (keyword.text == "unit") {
            expect(":");
            model.unit = type_name().text;
        } else if (keyword.text == "modular") {
            lex.boolean_enabled = true;
            if (accept("#true")) model.modular = true;
            else expect("#false");
            lex.boolean_enabled = false;
        }
        auto semi = expect(";");
        if (keyword.text == "unit") model.unit_span = span(dot.at, semi.at + 1);
    }

    void import(std::size_t at) {
        Import entry;
        entry.schema = matches("schema") && peek(1).kind == TokenKind::identifier &&
                       peek(1).text != "as" && peek(1).text != "from";
        if (entry.schema) take();
        auto first = predicate(qualified(identifier("an import name")), false);
        Token alias;
        if (accept("as")) alias = predicate(identifier("an import alias"), false);
        if (matches(",") || matches("from")) {
            for (;;) {
                if (first.text.find('.') != std::string::npos)
                    fail(first, "expected an unqualified selective import name");
                auto end = alias.text.empty() ? first.at + first.text.size() :
                                                alias.at + alias.text.size();
                if (entry.schema && (first.text.back() == '!' ||
                                     (!alias.text.empty() && alias.text.back() == '!')))
                    fail(first, "a schema import cannot import a Lua predicate");
                if (!alias.text.empty() && (first.text.back() == '!') != (alias.text.back() == '!'))
                    fail(alias, "predicate import aliases must retain '!'");
                entry.names.push_back({first.text, alias.text, span(first.at, end)});
                if (!accept(",")) break;
                first = predicate(identifier("an import name"), false);
                alias = {};
                if (accept("as")) alias = predicate(identifier("an import alias"), false);
            }
            expect("from");
            entry.module = qualified(identifier("an import module")).text;
        } else {
            if (first.text.back() == '!')
                fail(first, "expected 'from' for a predicate import");
            if (!alias.text.empty() && alias.text.back() == '!')
                fail(alias, "a module alias cannot be a predicate");
            entry.module = std::move(first.text);
            entry.alias = std::move(alias.text);
        }
        entry.span = span(at, expect(";").at + 1);
        model.imports.push_back(std::move(entry));
    }

    std::vector<std::string> parameters() {
        static const std::set<std::string> reserved{
            "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
            "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
            "then", "true", "until", "while"};
        std::vector<std::string> names;
        std::set<std::string> seen;
        expect("(");
        if (accept(")")) return names;
        do {
            auto id = identifier("a Lua parameter or result name", 0, true);
            if (reserved.count(id.text)) fail(id, "reserved Lua keyword '" + id.text + "'");
            if (!seen.insert(id.text).second) fail(id, "duplicate signature name '" + id.text + "'");
            names.push_back(std::move(id.text));
        } while (accept(","));
        expect(")");
        return names;
    }
    // Inspect only layout on the signature line. General lookahead would
    // tokenize the first Lua body token as ASDL (or reject it).
    bool signature_arrow() const {
        auto at = lex.pos;
        while (horizontal(lex.ch(at))) ++at;
        return lex.starts(at, "->");
    }
    void logistics(std::size_t at) {
        Logistics entry;
        expect("for");
        expect(":");
        entry.type = type_name().text;
        expect("::=");
        expect("lua");
        entry.parameters = parameters();
        if (entry.parameters.empty()) Lexer::fail(at, "logistics requires a node parameter");
        if (signature_arrow()) {
            expect("->");
            entry.synthesized = parameters();
        }
        entry.body = fragment(lex.lua_body(at));
        expect(".");
        auto end = expect("end");
        auto finish = end.at + end.text.size();
        // Only an optional semicolon on this line belongs to .end.
        auto tail = lex.pos;
        while (horizontal(lex.ch(tail))) ++tail;
        if (lex.ch(tail) == ';') finish = expect(";").at + 1;
        entry.span = span(at, finish);
        model.logistics.push_back(std::move(entry));
    }
};
} // namespace

Schema parse(std::string_view text, std::string_view file) {
    return Parser(text, file).run();
}

} // namespace asdl
