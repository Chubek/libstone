#include "wasmutils/WIT-Visitor.hpp"
#include "wasmutils/WITParser.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace wasm = limestone::wasmutils;
namespace {

void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void rejects(std::string_view source, std::string_view diagnostic) {
    try {
        wasm::parse_wit(source);
    } catch (const wasm::WitError& error) {
        if (std::string_view(error.what()).find(diagnostic) != std::string_view::npos)
            return;
        throw std::runtime_error("unexpected diagnostic for `" + std::string(source) +
                                 "`: " + error.what());
    }
    throw std::runtime_error("accepted invalid WIT: " + std::string(source));
}

void check_model() {
    const auto doc = wasm::parse_wit(R"WIT(
/// Program API
package demo:net-core@0.2.1-rc.1+meta;
use vendor:dependencies/remote@4.1.0 as remote;
use types as local;

world application {
    include base with { fetch as fallback, }
    use local.{ response, };
    type output = option<response>;
    import vendor:dependencies/logging@4.1.0;
    /// Exported convenience API.
    export status: func() -> output;
    export runner: interface {
        use types.{ connection };
        start: func(input: borrow<connection>);
    }
}

/** Types for the API. */
@since(version = 0.2.0, feature = transport)
interface types {
    use remote.{ endpoint as upstream, };
    connect: func(name: string, endpoint: upstream) -> result<connection, error-code>;
    type response = result<_, error-code>;
    record packet {
        /// Human-readable data, e.g. café.
        data: string,
        raw: list<u8>,
        meta: option<tuple<u32, error-code,>>,
    }
    variant event { empty, received(packet), failed(error-code), }
    enum error-code { missing, disconnected, }
    flags modes { fast, safe, }
    @unstable(feature = "more")
    resource connection {
        constructor(target: string);
        read: func() -> result<list<u8>>;
        open: static func(target: string) -> own<connection>;
    }
    resource buffer;
    ping: func() -> result;
}

world base {
    import fetch: func() -> string;
}
)WIT");

    using Kind = wasm::WitTypeKind;
    check(doc.package->version->text == "0.2.1-rc.1+meta" &&
          doc.package->annotations.docs.size() == 1, "package version or docs lost");
    check(doc.uses.size() == 2 && doc.uses[0].path.package &&
          doc.uses[1].alias->text == "local", "wrong top-level uses");

    const auto& iface = doc.interfaces.at(0);
    check(iface.name.text == "types" && iface.types.size() == 7 &&
          iface.functions.size() == 2, "wrong interface members");
    check(iface.annotations.docs.front().text == " Types for the API. " &&
          iface.annotations.attributes.front().arguments[0].value == "0.2.0",
          "wrong interface docs or attributes");
    check(iface.uses[0].items[0].alias->text == "upstream", "type import rename missing");
    check(iface.functions[0].parameters[0].type.kind == Kind::String &&
          iface.functions[0].result->arguments[0].name->text == "connection" &&
          iface.functions[1].result->arguments.empty(), "wrong function types");
    check(iface.types[0].alias->arguments[0].kind == Kind::Unit &&
          iface.types[0].alias->arguments[1].name->text == "error-code",
          "missing error-only result");
    const auto& packet = iface.types[1];
    check(packet.fields[0].annotations.docs[0].text.find("café") != std::string::npos &&
          packet.fields[1].type.kind == Kind::List &&
          packet.fields[2].type.arguments[0].kind == Kind::Tuple, "wrong record type");
    check(iface.types[2].cases[0].payload == std::nullopt &&
          iface.types[2].cases[1].payload->name->text == "packet", "wrong variant payload");
    check(iface.types[3].kind == wasm::WitTypeDefKind::Enum &&
          iface.types[4].kind == wasm::WitTypeDefKind::Flags, "wrong enum or flags model");
    const auto& resource = iface.types[5];
    check(resource.kind == wasm::WitTypeDefKind::Resource && resource.functions.size() == 3 &&
          resource.functions[0].kind == wasm::WitFunctionKind::Constructor &&
          resource.functions[1].kind == wasm::WitFunctionKind::Method &&
          resource.functions[2].kind == wasm::WitFunctionKind::Static &&
          resource.functions[2].result->kind == Kind::Own &&
          iface.types[6].functions.empty(), "wrong resource shape");

    const auto& world = doc.worlds.at(0);
    check(doc.worlds.size() == 2 && world.includes[0].renames[0].alias->text == "fallback",
          "wrong world include");
    check(world.items[0].is_import && world.items[0].path->package->version->text == "4.1.0" &&
          !world.items[1].is_import && world.items[1].function->result->name->text == "output" &&
          world.items[1].annotations.docs.size() == 1, "wrong world path or function import/export");
    check(world.items[2].interface->functions[0].parameters[0].type.kind == Kind::Borrow,
          "wrong inline world interface");

    struct Visitor final : wasm::WitVisitor {
        std::vector<std::string> items;
        unsigned args{};
        unsigned unit{};
        void visit(const wasm::WitStandaloneUse&) override { items.emplace_back("standalone"); }
        void visit(const wasm::WitInclude&) override { items.emplace_back("include"); }
        void visit(const wasm::WitInterface& item) override { items.push_back(item.name.text); }
        void visit(const wasm::WitWorld& item) override { items.push_back(item.name.text); }
        void visit(const wasm::WitFunction& item) override { items.push_back(item.name.text); }
        void visit(const wasm::WitTypeDef& item) override { items.push_back(item.name.text); }
        void visit(const wasm::WitAttributeArgument&) override { ++args; }
        void visit(const wasm::WitType& item) override { unit += item.kind == Kind::Unit; }
    } visitor;
    wasm::walk_wit(doc, visitor);
    check(visitor.items == std::vector<std::string>{
              "standalone", "standalone", "application", "include", "output", "status",
              "runner", "start", "types", "connect", "response", "packet", "event",
              "error-code", "modes", "connection", "constructor", "read", "open",
              "buffer", "ping", "base", "fetch"} &&
          visitor.args == 3 && visitor.unit == 1, "visitor lost nodes or their source order");
}

void check_validation() {
    auto doc = wasm::parse_wit(R"WIT(
package demo:types;
interface files {
    resource file;
    type a = list<b>;
    type b = tuple<own<file>, string>;
}
interface read {
    use demo:types/files.{ file as fd, a };
    f: func(v: a, reader: borrow<fd>) -> fd;
}
world main { include shared; export read; }
world shared { export read; }
)WIT");
    check(doc.worlds.size() == 2, "expected two worlds");
    auto comments = wasm::parse_wit(
        "interface a { f: /** ignored */ func(n: /** ignored */ u8, /** ignored */);"
        " /** ignored */ } /// detached");
    check(comments.interfaces[0].functions[0].annotations.docs.empty(),
          "detached doc comments must remain whitespace");

    rejects("package demo:bad@01.0.0; interface a {}", "SemVer");
    rejects("package demo:bad@1.0.0-rc.01; interface a {}", "SemVer");
    rejects("interface a { @since(feature = a, feature = b) f:func(); }", "duplicate attribute");
    rejects("interface a { type x = missing; }", "unknown WIT type");
    rejects("interface a { type long-name = u8; f: func(arg: longname); }", "unknown WIT type");
    rejects("interface a { type x = list<x>; }", "cyclic WIT type");
    rejects("interface a { record x { y: y } variant y { z(x) } }", "cyclic WIT type");
    rejects("interface a { type x = string; f: func(h: own<x>); }", "must be a resource");
    rejects("interface a { resource r; type x = borrow<r>; f: func() -> option<x>; }",
            "cannot return a borrowed handle");
    rejects("interface a { type x = list<>; }", "identifier");
    rejects("interface a { type x = result<_>; }", "expected ','");
    rejects("interface a { type x = tuple<>; }", "invalid WIT type arguments");
    rejects("interface a { enum e {} }", "at least one case");
    rejects("interface a { variant v { empty, EMPTY } }", "duplicate case");
    rejects("interface a { record r { x: u8, X: u16 } }", "duplicate record field");
    rejects("interface a { resource r { constructor(); constructor(); } }", "duplicate WIT resource constructor");
    rejects("interface a { resource r { a:func(self: u8); } }", "duplicate parameter");
    rejects("interface a { resource r { R:func(); } }", "cannot share its resource's name");
    rejects("interface a { use missing.{ x }; }", "unknown WIT interface or world");
    rejects("interface a { use b.{ f }; } interface b { f:func(); }", "unknown WIT type");
    rejects("interface a { use b.{ x }; type x = bool; } interface b { type x = u8; }",
            "duplicate interface or world item");
    rejects("interface a { use b.{ y }; type x = u8; } interface b { use a.{ x }; type y = u8; }",
            "cyclic WIT interface use");
    rejects("use first as second; use second as first;", "cyclic top-level use");
    rejects("interface a {} world A {}", "duplicate top-level");
    rejects("interface a {} use a;", "duplicate top-level");
    rejects("world x { include y; } world y { include x; }", "cyclic WIT world include");
    rejects("world x { include a; } interface a {}", "expected a WIT world");
    rejects("world x { import y; } world y {}", "expected a WIT interface");
    rejects("world x { include y with { a as b } } world y {}", "unknown named WIT include item");
    rejects("world x { include y with { A as b } } world y { import a: func(); }",
            "unknown named WIT include item");
    rejects("world x { import dep:lib/a; include dep:lib/a; }",
            "used as both an interface and a world");
    rejects("world x { include y; import a:func(); } world y { import a:func(); }",
            "duplicate named item");

    auto malformed = doc;
    malformed.interfaces[0].types[1].alias->arguments.clear();
    try {
        wasm::validate_wit(malformed);
        throw std::runtime_error("accepted manually constructed malformed list");
    } catch (const wasm::WitError& error) {
        check(std::string_view(error.what()).find("invalid WIT type arguments") != std::string_view::npos,
              "unexpected manually constructed AST diagnostic");
    }

    constexpr std::string_view invalid = "interface a {\r\n  /** café */ f: func() -> missing;\r\n}";
    try {
        wasm::parse_wit(invalid);
        throw std::runtime_error("accepted a bad type reference");
    } catch (const wasm::WitError& error) {
        check(error.location.offset == invalid.find("missing") && error.location.line == 2 &&
              error.location.column == invalid.find("missing") - invalid.find('\n'),
              "expected one-based line and byte column with CRLF and UTF-8");
    }

    constexpr std::string_view duplicate = "interface a {\n"
        "  first: func();\n"
        "  type FIRST = string;\n"
        "}";
    try {
        wasm::parse_wit(duplicate);
        throw std::runtime_error("accepted a duplicate between different kinds of declaration");
    } catch (const wasm::WitError& error) {
        check(error.location.offset == duplicate.find("FIRST"),
              "duplicate diagnostic must point to the later declaration");
    }

    std::string nested = "interface a { type t = ";
    for (unsigned i = 0; i < 129; ++i)
        nested += "list<";
    nested += "u8";
    nested.append(129, '>');
    nested += "; }";
    rejects(nested, "nesting is too deep");
}

} // namespace

int main() {
    try {
        check_model();
        check_validation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
