#include <smtlib2/ast.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace smtlib2;
using T = TokenKind;
using K = CommandKind;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Action>
Span rejects(Action action, ErrorCategory category, std::string_view source) {
    try {
        action();
    } catch (const Error& error) {
        if (error.category() != category)
            throw std::runtime_error("wrong error category for: " + std::string(source));
        return error.span();
    }
    throw std::runtime_error("accepted invalid input: " + std::string(source));
}

void position(Position actual, std::size_t offset, std::size_t line, std::size_t column) {
    require(actual.offset == offset && actual.line == line && actual.column == column, "unexpected source position");
}

std::string_view slice(std::string_view source, Span span) {
    require(span.begin.offset <= span.end.offset && span.end.offset <= source.size(), "invalid byte range");
    return source.substr(span.begin.offset, span.end.offset - span.begin.offset);
}

// SMT-LIB 2.6, sections 3.1 and 3.2 (2024-09-20 revision).
void lexicon() {
    const struct { const char* text; T kind; } examples[] = {
        {"(", T::lparen}, {")", T::rparen}, {"0", T::numeral},
        {"987654321098765432109876543210987654321", T::numeral},
        {"0.0", T::decimal}, {"123.00042", T::decimal}, {"7.000", T::decimal},
        {"#b01", T::binary}, {"#x0aF", T::hexadecimal},
        {"~!@$%^&*_-+=<>.?/Az9", T::atom}, {"-32", T::atom}, {".8", T::atom},
        {"let1", T::atom}, {"ASSERT", T::atom},
        {":date", T::keyword}, {":my-attr2", T::keyword}, {"||", T::quoted_symbol},
        {"|assert|", T::quoted_symbol}, {"|x ; (\" )\r\n\t y|", T::quoted_symbol},
        {"|\xc3\xa9|", T::quoted_symbol}, {"|\xe9|", T::quoted_symbol},
        {"\"\"", T::string_literal}, {"\"\"\"\"", T::string_literal},
        {"\"\xc3\xa9\"", T::string_literal}, {"\"\xe9\"", T::string_literal},
        {R"("x""y\z")", T::string_literal},
        {"BINARY", T::reserved}, {"DECIMAL", T::reserved}, {"HEXADECIMAL", T::reserved},
        {"NUMERAL", T::reserved}, {"STRING", T::reserved}, {"_", T::reserved},
        {"!", T::reserved}, {"as", T::reserved}, {"let", T::reserved},
        {"exists", T::reserved}, {"forall", T::reserved}, {"match", T::reserved}, {"par", T::reserved}
    };
    for (const auto& example : examples) {
        const auto tokens = lex(example.text);
        require(tokens.size() == 1 && tokens[0].kind == example.kind && tokens[0].text == example.text,
                "unexpected token classification or spelling");
        position(tokens[0].span.begin, 0, 1, 1);
        require(slice(example.text, tokens[0].span) == example.text, "token did not cover its entire spelling");
    }

    for (const auto* text : {"00", "012.3", "1.", "1.2.3", "12x", "#b", "#x", "#b102", "#xABg", "#X1",
                             ":", ":1st", ":let", ":assert", "a:b", "foo\\bar", "a\"b\"", "a|b|",
                              "\"abc", "\"\"\"", "|abc", "|abc\\def|", "\v", "\"\x7f\"", "|\x7f|", "\xc3\xa9"}) {
        rejects([&] { lex(text); }, ErrorCategory::lexical, text);
    }
    const std::string nul{"\"x\0y\"", 5};
    auto bad = rejects([&] { lex(nul); }, ErrorCategory::lexical, nul);
    position(bad.begin, 2, 1, 3);
    position(bad.end, 3, 1, 4);
    require(lex(" \t;\")|comment without final newline").empty(), "comment leaked tokens");

    constexpr std::string_view international = ";\xe9\r\n(\"\xc3\xa9\" |\xe9|)";
    const auto tokens = lex(international);
    require(tokens.size() == 4, "unexpected token count for international text");
    position(tokens[1].span.begin, 5, 2, 2);
    position(tokens[1].span.end, 9, 2, 6);
    position(tokens[2].span.begin, 10, 2, 7);
    position(tokens[2].span.end, 13, 2, 10);
    require(print(parse(international).front()) == "(\"\xc3\xa9\" |\xe9|)",
            "international text was not preserved byte for byte");
}

void concrete_spans_and_limits() {
    constexpr std::string_view source = ";head\r\n (x\r(y)\n \"a\r\nb\") ;end";
    const auto tokens = lex(source);
    require(tokens.size() == 7, "unexpected token count");
    for (const auto& token : tokens)
        require(slice(source, token.span) == token.text, "token range did not match spelling");
    position(tokens.front().span.begin, 8, 2, 2);
    position(tokens[2].span.begin, 11, 3, 1);
    position(tokens[5].span.begin, 16, 4, 2);
    position(tokens[5].span.end, 22, 5, 3);

    const auto roots = parse(source);
    require(roots.size() == 1 && roots[0].children.size() == 3, "wrong S-expression shape");
    require(slice(source, roots[0].span) == "(x\r(y)\n \"a\r\nb\")", "list range did not include both parentheses");
    require(slice(source, roots[0].children[1].span) == "(y)", "wrong nested list range");
    require(print(roots[0]) == "(x (y) \"a\r\nb\")", "concrete printer altered a token");
    position(roots[0].span.end, 23, 5, 4);

    require(parse("(())", {4, 4, 2}).size() == 1, "exact resource limits should pass");
    require(parse("x", {1, 1, 0}).size() == 1, "depth must count lists, not atoms");
    require(parse("", {0, 0, 0}).empty(), "zero budget should accept empty input");
    rejects([] { parse("(())", {3, 4, 2}); }, ErrorCategory::resource_limit, "(())");
    auto bad = rejects([] { parse("(())", {4, 3, 2}); }, ErrorCategory::resource_limit, "(())");
    position(bad.begin, 3, 1, 4);
    position(bad.end, 3, 1, 4);
    bad = rejects([] { parse("(())", {4, 4, 1}); }, ErrorCategory::resource_limit, "(())");
    position(bad.begin, 1, 1, 2);
    position(bad.end, 2, 1, 3);
    bad = rejects([] { parse("\r\n())"); }, ErrorCategory::syntax, "\\r\\n())");
    position(bad.begin, 4, 2, 3);
    position(bad.end, 5, 2, 4);
    bad = rejects([] { parse("(\r\n (x"); }, ErrorCategory::syntax, "(\\r\\n (x");
    position(bad.begin, 4, 2, 2);
    position(bad.end, 5, 2, 3);

    const Limits defaults;
    const auto opening = std::string(defaults.max_depth, '(');
    const auto closing = std::string(defaults.max_depth, ')');
    const auto deep = opening + "x" + closing;
    require(print(parse(deep).front()) == deep, "round trip failed at maximum list depth");
    bad = rejects([&] { parse("(" + deep + ")"); }, ErrorCategory::resource_limit, "deep nesting");
    require(bad.begin.offset == defaults.max_depth, "depth failure at wrong opening parenthesis");

    std::string nested_term = "(assert ";
    for (std::size_t i = 1; i < defaults.max_depth; ++i) nested_term += "(f ";
    nested_term += 'x';
    nested_term += closing;
    auto script = parse_script(nested_term);
    require(script.commands.front().arena.terms.size() == defaults.max_depth && print(script) == nested_term,
            "typed arena did not round-trip a term at the default depth limit");
}

// Independent examples for all scripting commands, exercising typed syntax
// without assuming name resolution, supported theories, or execution order.
void scripts_and_typed_nodes() {
    const struct { const char* text; K kind; } examples[] = {
        {"(set-logic ALL)", K::set_logic},
        {"(set-info :smt-lib-version 2.6)", K::set_info},
        {R"((set-info :source (meta :tag () (_ f 2) (as f Int) (assert p) "a""b")))", K::set_info},
        {"(set-option :produce-models true)", K::set_option},
        {"(declare-sort S 1)", K::declare_sort},
        {"(define-sort A (U) (Array U (_ BitVec 16)))", K::define_sort},
        {"(declare-const |assert| (A Int))", K::declare_const},
        {"(declare-fun g (Int Int) Int)", K::declare_fun},
        {"(declare-datatype T ((nil) (box (contents Int))))", K::declare_datatype},
        {"(declare-datatypes ((L 1) (D 0)) ((par (X) ((empty) (cons (head X) (tail (L X))))) ((d))))", K::declare_datatypes},
        {"(define-fun f ((x Int)) Int x)", K::define_fun},
        {"(define-fun-rec r ((x Int)) Int (r x))", K::define_fun_rec},
        {"(define-funs-rec ((even ((x Int)) Bool) (odd ((x Int)) Bool)) ((odd x) (even x)))", K::define_funs_rec},
        {"(assert (! (let ((x 1) (y x)) (forall ((z Int)) (exists ((w Int)) (p x y z w)))) :flag :named p1))", K::assert_},
        {"(assert (match t ((nil 0) ((box x) x))))", K::assert_},
        {"(assert ((as (_ f 1 |i x|) (Array Int Int)) #b1 #xAf 1.01))", K::assert_},
        {"(check-sat)", K::check_sat},
        {"(check-sat-assuming (x (not |assert|)))", K::check_sat_assuming},
        {"(check-sat-assuming ())", K::check_sat_assuming},
        {R"((echo "one ""quote"""))", K::echo},
        {"(get-assertions)", K::get_assertions}, {"(get-assignment)", K::get_assignment},
        {"(get-info :name)", K::get_info}, {"(get-model)", K::get_model},
        {"(get-option :print-success)", K::get_option}, {"(get-proof)", K::get_proof},
        {"(get-unsat-assumptions)", K::get_unsat_assumptions}, {"(get-unsat-core)", K::get_unsat_core},
        {"(get-value (x (as nil (L Int))))", K::get_value},
        {"(push 2)", K::push}, {"(pop 2)", K::pop},
        {"(reset-assertions)", K::reset_assertions}, {"(reset)", K::reset}, {"(exit)", K::exit}
    };
    std::string source, expected;
    for (const auto& example : examples) {
        source += "\t; ignored comment\r\n  ";
        source += example.text;
        source += '\r';
        if (!expected.empty()) expected += '\n';
        expected += example.text;
    }
    const auto script = parse_script(source);
    const auto again = parse_script(print(script));
    require(script.commands.size() == sizeof(examples) / sizeof(*examples), "wrong command count");
    require(print(again) == expected && print(script) == expected, "script did not round-trip");
    for (std::size_t i = 0; i < script.commands.size(); ++i) {
        const auto& command = script.commands[i];
        require(command.kind == examples[i].kind && again.commands[i].kind == command.kind, "wrong command variant");
        require(command.syntax.children[0].kind == T::reserved, "command token not reserved");
        require(slice(source, command.syntax.span) == examples[i].text, "wrong command span");
    }

    const auto& info = std::get<AttributeSyntax>(script.commands[2].value);
    require(info.value && info.value->children[1].kind == T::keyword
                && info.value->children[2].children.empty()
                && info.value->children[3].children[0].kind == T::reserved
                && info.value->children[5].children[0].kind == T::reserved,
            "nested attribute lost an empty list, keyword, or reserved word");
    require(slice(source, info.span) == R"(:source (meta :tag () (_ f 2) (as f Int) (assert p) "a""b"))",
            "wrong valued attribute span");

    const auto& command = script.commands[13];
    const auto& arena = command.arena;
    const auto& annotation = std::get<AnnotationSyntax>(arena[std::get<TermSyntaxId>(command.value)].value);
    require(annotation.attributes.size() == 2 && !annotation.attributes[0].value
                && slice(source, annotation.attributes[0].span) == ":flag"
                && slice(source, annotation.attributes[1].value->span) == "p1",
            "flag attribute swallowed the next attribute");
    const auto& let = std::get<LetSyntax>(arena[annotation.term].value);
    require(let.bindings.size() == 2 && slice(source, let.bindings[1].span) == "(y x)", "wrong let binding");
    const auto& forall = std::get<QuantifierSyntax>(arena[let.body].value);
    const auto& exists = std::get<QuantifierSyntax>(arena[forall.body].value);
    require(forall.kind == QuantifierSyntax::Kind::forall && exists.kind == QuantifierSyntax::Kind::exists
                && slice(source, arena[exists.variables[0].sort].span) == "Int",
            "wrong quantifier or sorted variable");

    const auto& match_command = script.commands[14];
    const auto& match = std::get<MatchSyntax>(match_command.arena[std::get<TermSyntaxId>(match_command.value)].value);
    require(match.cases.size() == 2 && match.cases[0].pattern.variables.empty()
                && match.cases[1].pattern.variables[0].name() == "x", "wrong match pattern");
    const auto& app_command = script.commands[15];
    const auto& app = std::get<ApplicationSyntax>(app_command.arena[std::get<TermSyntaxId>(app_command.value)].value);
    require(app.function.sort && app.arguments.size() == 3
                && std::get<NumeralSyntax>(app.function.identifier.indices[0]).spelling == "1"
                && std::get<SymbolSyntax>(app.function.identifier.indices[1]).name() == "i x"
                && std::get<ConstantSyntax>(app_command.arena[app.arguments[1]].value).kind == T::hexadecimal,
            "wrong indexed identifier, qualification, or constant");
    const auto& assumptions = std::get<std::vector<PropositionSyntax>>(script.commands[17].value);
    require(!assumptions[0].negated && assumptions[1].negated && assumptions[1].symbol.quoted
                && assumptions[1].symbol.name() == "assert", "wrong assumption literal");

    auto sort = make_sort(parse("((_ F 3) (_ BitVec 8) |let|)").front());
    const auto sort_copy = sort;
    const auto& node = sort_copy.arena[sort_copy.root];
    require(print(sort_copy) == "((_ F 3) (_ BitVec 8) |let|)" && node.arguments.size() == 2
                && sort_copy.arena[node.arguments[1]].identifier.symbol.name() == "let",
            "standalone sort did not retain its child IDs after copy");
    const auto term = make_term(parse("(as x (_ BitVec 8))").front());
    const auto& qualified = std::get<QualifiedIdentifierSyntax>(term.arena[term.root].value);
    require(qualified.sort && print(term) == "(as x (_ BitVec 8))", "standalone qualified term did not round-trip");
}

void rejects_bad_grammar() {
    for (const auto* text : {
             "bare", "()", "(|assert| true)", "(unknown)", "(check-sat arg)", "(assert :key)",
             "(assert (f))", "(assert (let () true))", "(assert (forall () true))", "(assert (let ((x)) x))",
             "(assert (! a))", "(assert (! a named))", "(assert (! a :key let))", "(assert (_ f))",
             "(assert (_ f 1.5))", "(assert (match t ()))", "(assert (match t (((box) 0))))",
             "(declare-const 1 Int)", "(declare-const assert Bool)", "(declare-const x (Int))",
             "(declare-datatype T ())", "(declare-datatypes () ())", "(define-funs-rec () ())",
             "(declare-datatypes ((T 0) (U 0)) (((a))))", "(declare-datatypes ((T 0)) (((a)) ((b))))",
             "(define-funs-rec ((f () Bool) (g () Bool)) (true))", "(define-funs-rec ((f () Bool)) (true false))",
             "(get-value ())", "(echo x)", "(check-sat-assuming ((not (f x))))",
             "(set-info :x :y)", "(set-info :x assert)", "(push x)", "(assert (as a))", "(assert reset)"}) {
        rejects([&] { parse_script(text); }, ErrorCategory::syntax, text);
    }
    constexpr std::string_view text = ";comment\r\n (assert (! x :tag forall))";
    auto bad = rejects([&] { parse_script(text); }, ErrorCategory::syntax, text);
    require(slice(text, bad) == "forall", "typed diagnostic did not point to the offending token");
    position(bad.begin, 29, 2, 20);
    position(bad.end, 35, 2, 26);

    constexpr std::string_view mismatch = "(define-funs-rec ((f () Bool)) (true false))";
    bad = rejects([&] { parse_script(mismatch); }, ErrorCategory::syntax, mismatch);
    require(slice(mismatch, bad) == "(true false)", "mismatched recursive group diagnostic missed the body list");
}
}

int main() {
    try {
        lexicon();
        concrete_spans_and_limits();
        scripts_and_typed_nodes();
        rejects_bad_grammar();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
