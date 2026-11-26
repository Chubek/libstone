#include "asdl/schema.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
namespace fs = std::filesystem;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

asdl::Schema checked(const fs::path &path, const std::vector<fs::path> &roots = {}) {
    auto schema = asdl::parse_file(path, roots);
    asdl::validate(schema);
    return schema;
}

template <typename Operation>
asdl::Diagnostic diagnostic(Operation operation, std::string_view text) {
    try {
        operation();
    } catch (const asdl::Diagnostic &error) {
        require(error.message.find(text) != std::string::npos, error.what());
        return error;
    }
    std::cerr << "expected failure containing: " << text << '\n';
    std::exit(1);
}

template <typename Operation>
void rejected(Operation operation, std::string_view text) {
    (void)diagnostic(operation, text);
}

void legacy(const fs::path &fixtures) {
    auto calc = checked(fixtures / "calculator.asdl");
    require(calc.module == "Calculator" && calc.prefix == "calc_", "Perl fixtures: module/prefix");
    require(calc.prologue->text.find("CALC_FIXTURE_VERSION") != std::string::npos &&
            calc.epilogue->text.find("_Static_assert") != std::string::npos,
            "Perl fixtures: raw fragments");
    require(calc.generics.count("map[int,expr]") && calc.generics.count("array[expr,2]") &&
            calc.generics.count("list[expr]"), "Perl fixtures: containers");
    require(calc.definitions[0].fields[0].qualifier == '+', "Perl fixtures: nonempty");
    rejected([&] { checked(fixtures / "syntax-error.asdl"); }, "expected a lowercase field type");
    const auto missing = diagnostic([&] { checked(fixtures / "unknown-type.asdl"); }, "undefined type");
    require(missing.span.begin.line == 4 && missing.span.begin.column == 18,
            "Perl fixtures: positions after initial %%");
    rejected([&] { checked(fixtures / "recursive-product.asdl"); }, "recursive product type");

    auto schema = asdl::parse(R"ASDL(%{
// keep commenting \
%}
static const char *p = "%}";
/* keep commenting %}
*/
%}
%option header="a file.h" prefix=test no-enums omit-builtin=list
%no-builtin map
%%
#false is a legacy comment here
module = Thing
attributes = Many(array[list[identifier],0002])
%%
%%
)ASDL", "envelope.asdl");
    require(schema.options.at("header") == "a file.h" &&
            schema.options.at("enums") == "0" &&
            schema.options.at("omit-builtin") == "list,map", "legacy option syntax");
    require(schema.definitions.size() == 2 && schema.generics.count("array[list[identifier],2]"),
            "contextual keywords and canonical container size");
    require(schema.prologue->text.find("static const") != std::string::npos &&
            schema.epilogue->text == "\n%%\n", "comments/strings and passthrough epilogue");
    rejected([] { asdl::parse("%prefix here\nroot = A"); }, "expected initial %%");
    rejected([] { asdl::parse("root = A\n%%\n"); }, "epilogue requires an initial %%");
    rejected([] { asdl::parse("root = A %%\n"); }, "must occupy a line");
    rejected([] { asdl::parse("%option no-enums=false\n%%\nroot = A"); }, "does not take a value");
}

void extended(const fs::path &examples) {
    auto mini = checked(examples / "Mini.asdl");
    require(mini.language == "Mini" && mini.unit == "toplevel" && !mini.modular &&
            mini.logistics.size() == 1 && mini.imports.size() == 3, "Mini metadata");
    const auto &mini_cons = mini.constructors.at("Conditional");
    const auto &cond = mini.definitions[mini_cons.definition].constructors[mini_cons.constructor].fields[0];
    require(cond.refinement == "foldable!" && cond.executable_refinement, "Mini executable refinement");
    auto ml = checked(examples / "MiniML.asdl");
    require(ml.version == "0.1.0" && ml.logistics.size() == 9 &&
            ml.logistics.at(2).parameters.size() == 5, "MiniML multiline inherited signature");
    auto ruby = checked(examples / "MiniRuby.asdl");
    require(ruby.logistics.size() == 14 && ruby.imports.size() == 6, "MiniRuby callbacks and imports");
    const auto &range_cons = ruby.constructors.at("Range");
    const auto &range = ruby.definitions[range_cons.definition].constructors[range_cons.constructor];
    require(range.fields.back().name == "exclusive", "MiniRuby constructor fields");
    require(ruby.definitions[ruby.types.at("method")].fields[1].refinement == ":method",
            "MiniRuby nominal refinement");

    auto schema = asdl::parse(
        ".language 'Lexing';\r\n"
        ".modular #false;\r\n"
        ".unit :root;\r\n"
        ".import std.rewrite as R;\r\n"
        ".import foldable! as safe! from std.rewrite;\r\n"
        "root ::= Node(int<R.foldable!>? first, int<safe!> second);\r\n"
        ".logistics for :root ::= lua(_node, _ctx) -> (value, metadata)\r\n"
        "    local x = [==[\r\n"
        ".end\r\n"
        "]===] is not the closing delimiter\r\n"
        "]==]\r\n"
        "    --[=[ a long comment\r\n"
        "    .end\r\n"
        "    ]=]\r\n"
        "    -- .end\r\n"
        "    local s = \"escaped quote \\\" then .end and continued \\\r\n"
        "    still in the string\"\r\n"
        "    local t = 'escaped whitespace \\z\r\n"
        "    continued'\r\n"
        "    return x, nil\r\n"
        "  .end; -- real terminator\r\n"
        "tail ::= Tail;\r\n", "lexing.asdl");
    asdl::resolve(schema);
    asdl::validate(schema);
    const auto &hook = schema.logistics.at(0);
    require(hook.parameters == std::vector<std::string>{"_node", "_ctx"} &&
            hook.synthesized == std::vector<std::string>{"value", "metadata"},
            "explicit signature and Lua identifier parameters");
    require(hook.body.span.begin.line == 7 && hook.body.span.end.line == 21 &&
            hook.body.text.find("\r\n.end\r\n") != std::string::npos &&
            hook.body.text.find("return x, nil") != std::string::npos,
            "Lua body keeps CRLF and quoted/commented terminators");
    require(schema.definitions[1].span.begin.line == 22, "correct lexer resume position");

    rejected([] { asdl::parse("root ::= Leaf; .unit :root; .unit :root;"); }, "duplicate directive");
    rejected([] { asdl::parse("root ::= Leaf"); }, "expected ';'");
    rejected([] { asdl::parse("root = Leaf\n.logistics for :root ::= lua(a,a)\n.end"); },
             "duplicate signature name");
    rejected([] { asdl::parse("root = Leaf\n.logistics for :root ::= lua()\n.end"); },
             "requires a node");
    rejected([] { asdl::parse("root = Leaf\n.logistics for :root ::= lua(a)\nlocal s = [=[\n.end"); },
             "unterminated Lua long bracket");
    rejected([] { asdl::parse("root = Leaf\n.logistics for :root ::= lua(a)\nlocal s = 'bad\n.end"); },
             "unterminated Lua short string");
    rejected([] { asdl::parse("root = Leaf\n.logistics for :root ::= lua(a)\nlocal x = 0"); },
             "unterminated Lua logistics");
    rejected([] { auto s = asdl::parse(".unit :missing; root = Leaf"); asdl::resolve(s); },
             "undefined type 'missing'");
    rejected([] { auto s = asdl::parse("root = Leaf(int<check!> a)"); asdl::resolve(s); },
             "unimported executable refinement");
    rejected([] {
        auto s = asdl::parse(".import check! from m; .import check! from n; root = Leaf");
        asdl::resolve(s);
    }, "duplicate Lua import binding");
}

void modules(const fs::path &fixtures) {
    auto entry = fixtures / "modular";
    auto schema = checked(entry / "main.asdl", {entry / "modules"});
    require(schema.definitions.size() == 5 && schema.types.count("root"), "diamond loads once");
    const auto &root = schema.definitions.front();
    require(root.fields.at(0).type == schema.unit &&
            root.fields.at(1).type == root.fields.at(2).type &&
            root.fields.at(0).type != root.fields.at(1).type, "namespace and selective name rewriting");
    require(schema.generics.at(root.fields.at(3).type).args.at(0) == root.fields.at(1).type,
            "qualified reference inside generic");
    require(schema.logistics.size() == 1 && schema.logistics[0].type == schema.unit &&
            schema.logistics[0].span.file.find("nodes.asdl") != std::string::npos,
            "imported hook target identity and origin");
    require(schema.imports.at(0).resolved_file == schema.logistics[0].span.file &&
            schema.imports.at(4).span.file == schema.imports[0].resolved_file &&
            schema.imports[4].resolved_file == schema.imports.at(1).resolved_file,
            "direct and transitive imports retain both sides of each graph edge");
    asdl::resolve(schema);
    require(schema.definitions.size() == 5, "resolution is idempotent");

    auto cycle = diagnostic([&] { checked(entry / "cycle_a.asdl"); }, "schema import cycle");
    require(cycle.span.file.find("cycle_b.asdl") != std::string::npos && cycle.span.begin.line == 2,
            "cycle located at closing import edge");
    rejected([&] { checked(entry / "main.asdl"); }, "not found");
    rejected([&] {
        auto s = asdl::parse(".import schema private; root = Leaf", (entry / "main.asdl").string());
        asdl::resolve(s);
    }, "must declare .modular #true");
    rejected([&] {
        auto s = asdl::parse("root = Leaf(nope.node)", (entry / "main.asdl").string());
        asdl::resolve(s);
    }, "unknown schema import alias");
}

} // namespace

int main(int argc, char **argv) {
    require(argc >= 2, "usage: grammar-test <path-to-cornerstone/ASDL> [additional schemas...]");
    try {
        auto source = fs::path(argv[1]);
        legacy(source / "tests" / "fixtures");
        extended(source / "examples");
        modules(source / "tests" / "fixtures");
        for (int i = 2; i < argc; ++i) checked(argv[i]);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
