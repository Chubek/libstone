#include "Frontend.hpp"

#include <iostream>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class F>
void rejects(F&& action, std::string_view message) {
    try {
        action();
    } catch (const am8::Error& error) {
        check(std::string_view(error.what()).find(message) != std::string_view::npos,
              error.what());
        return;
    }
    throw std::runtime_error("unexpected success; expected: " + std::string(message));
}

void lexical() {
    // Return an owned syntax object after both arguments have been destroyed.
    auto root = [] {
        const std::string source = "; heading\r\n(library Owned\n  (c \"a;()\\n\\r\\t\\\"\\\\\") ; skip\n)";
        const auto baseline = dsl::parse_bounded_sexpr(source);
        auto parsed = am8::parse(source, std::string("owned.am8"));
        check(am8::to_sexpr(parsed) == baseline, "lexical compatibility");
        check(parsed.span.end == source.size(), "outer end offset");
        const auto& node = parsed.items.at(2).items.at(1);
        check(node.span.begin == source.find('"'), "quoted atom begins at opening delimiter");
        check(source[node.span.end - 1] == '"', "range includes closing delimiter");
        return parsed;
    }();
    check(root.span.begin == 11 && root.span.line == 2 && root.span.column == 1, "root span");
    const auto& quoted = root.items.at(2).items.at(1);
    check(quoted.span.file == "owned.am8" && quoted.span.line == 3 && quoted.span.column == 6,
          "owned leaf location");
    check(quoted.quoted && quoted.text == "a;()\n\r\t\"\\", "decoded text");
    check(quoted.provenance.empty(), "unexpanded source has no provenance");

    for (std::string_view source : {"(\"\" bare\\token embedded\"quote)", "(;comment\n\t() a)\r",
                                   "\"line 1\nline 2\"", "(x\\y \"\\r\\n\")"}) {
        const auto tree = am8::to_sexpr(am8::parse(source, "roundtrip.am8"));
        check(tree == dsl::parse_bounded_sexpr(source), "LibDSL lexical semantics");
        check(tree == dsl::parse_bounded_sexpr(tree.dump()), "LibDSL lexical roundtrip");
    }
    rejects([] { am8::parse("(\n  \"a\\q\")", "bad.am8"); }, "bad.am8:2:6: unknown escape");
    rejects([] { am8::parse("(x)\n y", "bad.am8"); }, "bad.am8:2:2: expected exactly one");
    rejects([] { am8::parse("(\n x", "bad.am8"); }, "bad.am8:2:3: unterminated list");
    rejects([] { am8::parse(")", "bad.am8"); }, "bad.am8:1:1: unexpected ')'");
    rejects([] { am8::parse(" \t ", "bad.am8"); }, "bad.am8:1:4: expected expression");
    const auto multiline = am8::parse("(\"a\nb\" xyz)", "multiline.am8");
    check(multiline.items[1].span.line == 2 && multiline.items[1].span.column == 4,
          "literal newline updates byte coordinates");

    am8::Limits limits;
    limits.syntax_depth = 1;
    rejects([&] { am8::parse("(a (b))", "depth.am8", limits); }, "nesting limit exceeded");
    limits.syntax_nodes = 2;
    rejects([&] { am8::parse("(a b)", "nodes.am8", limits); }, "nodes.am8:1:4: syntax node");
    limits.source_bytes = 3;
    rejects([&] { am8::parse("(ab)", "bytes.am8", limits); }, "source byte limit");
    check(am8::parse("(a)", "bytes.am8", limits).items.size() == 1, "inclusive resource limits");
}

void modules(const am8::fs::path& fixtures) {
    const auto program = am8::load(fixtures / "root.am8");
    check(program.root == 2 && program.modules.size() == 3, "canonical dependency-first modules");
    const auto& leaf = program.modules[0];
    const auto& branch = program.modules[1];
    const auto& root = program.modules[2];
    check(leaf.name == "LeafModule" && branch.name == "BranchModule" &&
          root.name == "GrammarFixture", "module names");
    check(leaf.exports == std::set<std::string>{"leaf"} && leaf.forms.size() == 1 &&
          am8::head(leaf.forms[0]) == "fn", "only runtime forms remain");
    check(branch.imports.size() == 1 && branch.imports[0].target == 0, "nested relative import");
    check(root.imports.size() == 3, "duplicate canonical import deduplicated");
    check(root.imports[0].target == 1 && root.imports[0].alias == "Branch" &&
          root.imports[0].span.line == 3, "qualified import with span");
    check(root.imports[1].target == 0 && root.imports[1].alias.empty() &&
          root.imports[2].target == 0 && root.imports[2].alias == "Leaf", "distinct import bindings");

    auto source = [&](std::string_view text, const am8::Limits& limits = {}) {
        return am8::load(am8::parse(text, "<embedded>", limits), fixtures, limits);
    };
    auto embedded = source("(module Embedded (import \"leaf.am8\" L))");
    check(embedded.root == 1 && embedded.modules[1].span.file == "<embedded>" &&
          embedded.modules[1].imports[0].target == 0, "embedded relative imports");
    rejects([&] { am8::load(fixtures / "cycle.am8"); }, "cycle.am8:2:3: import cycle:");
    rejects([&] { source("(module M (import \"absent.am8\"))"); },
            "<embedded>:1:11: cannot open input");
    rejects([&] { source("(module M (import \"nested\"))"); }, "not a regular file");
    rejects([&] { source("(module M (import \"/leaf.am8\"))"); }, "relative import path");
    rejects([&] { source("(module M (import leaf.am8))"); }, "expected quoted text");
    rejects([&] { source("(module M (import \"leaf.am8\" \"L\"))"); }, "invalid identifier");
    rejects([&] { source("(module M (import \"leaf.am8\" L) (import \"nested/branch.am8\" L))"); },
            "duplicate import alias 'L'");
    rejects([&] { source("(module M (export foo) (export bar foo))"); }, "duplicate export 'foo'");
    rejects([&] { source("(module M (export))"); }, "expected (export NAME ...)");
    rejects([&] { source("(module)"); }, "expected (library NAME ...)");

    am8::Limits limits;
    limits.modules = 2;
    rejects([&] { am8::load(fixtures / "root.am8", limits); }, "module limit exceeded");
    limits.modules = 0;
    rejects([&] { source("(module M)", limits); }, "module limit exceeded");
    limits.modules = 3;
    const auto import = "(module M (import \"leaf.am8\"))";
    limits.syntax_nodes = 6; // The entire root fits, but leaves nothing for its import.
    rejects([&] { source(import, limits); }, "leaf.am8:1:1: syntax node limit exceeded");
    limits.syntax_nodes = 65536;
    limits.source_bytes = std::string_view(import).size() + 1;
    rejects([&] { source(import, limits); }, "<embedded>:1:11: source byte limit exceeded");
    limits.source_bytes = am8::fs::file_size(fixtures / "root.am8") +
                          am8::fs::file_size(fixtures / "nested/branch.am8") +
                          am8::fs::file_size(fixtures / "leaf.am8");
    check(am8::load(fixtures / "root.am8", limits).root == 2,
          "shared byte limit charges canonical inputs only once");
    --limits.source_bytes;
    rejects([&] { am8::load(fixtures / "root.am8", limits); }, "source byte limit exceeded");
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: W1Frontend FIXTURE_DIRECTORY");
        lexical();
        modules(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
