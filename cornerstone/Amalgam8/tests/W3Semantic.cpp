#include "Emitter.hpp"
#include "Frontend.hpp"
#include "Semantic.hpp"

#include <iostream>

namespace {

using namespace am8;

void check(bool condition, std::string_view message) {
    if (!condition) throw Error(std::string(message));
}

Program program(std::string_view source) {
    return load(parse(source, "W3.am8"), ".");
}

void rejects(std::string_view source, std::string_view diagnostic) {
    try {
        lower(program(source));
    } catch (const Error& e) {
        const std::string_view msg = e.what();
        check(msg.find(diagnostic) != msg.npos, msg);
        check(msg.find("W3.am8:") != msg.npos, msg);
        return;
    }
    throw Error("expected rejection: " + std::string(diagnostic));
}

std::size_t find(const Unit& unit, std::string_view name, bool body = false) {
    for (std::size_t i = 0; i < unit.declarations.size(); ++i) {
        const auto& d = unit.declarations[i];
        if (d.name == name && bool(d.body) == body) return i;
    }
    throw Error("missing declaration: " + std::string(name));
}

Unit typed_program() {
    auto unit = lower(program(R"am8(
        (library Test
          (include limits.h)
          (def Max INT_MAX)
          (fn int even ((int n))
            (if (== n 0) (return 1) (return (call odd (- n 1)))))
          (fn int odd ((int n))
            (if (== n 0) (return 0) (return (call even (- n 1)))))
          (fn int access (((ptr Outer) x))
            (return (index (field (ptr-field x inner) data) 1)))
          (record Outer ((Inner inner) ((ptr Outer) next)))
          (type Inner Payload)
          (record Payload (((array int 2) data)))
          (type Callback (ptr (function int (int))))
          (var Callback callback even)
          (var (array int 2) table (raw "{4, 8}"))
          (var (array char 3) letters "ok")
          (type Text (ptr (const char)))
          (fn bool yes () (return true))
          (fn uint32_t width ((size_t n)) (return n))
          (fn Text greet () (return "hello"))
          (extern int lib_print ((Text text)))
          (c "typedef int Foreign;")
          (fn "Foreign" opaque () "return 0;")
          (cpp "// opaque C++")
          (enum Color (Red (Green (+ Red 1))))
          (fn Color choose () (return Green))
          (fn int launch ((int argc) ("char **" argv))
            (block
              (let int v (call callback argc))
              (set (index table 1) v)
              (while (< v 2) (set v (+ v 1)))
              (if argv (return (+ (index table 0) v)) (return 1))))
          (cli launch))
    )am8"));
    check(find(unit, "even") < find(unit, "odd", true), "missing mutual-recursion prototype");
    check(find(unit, "odd") < find(unit, "even", true), "missing mutual-recursion prototype");
    check(find(unit, "Inner") < find(unit, "Outer"), "missing alias dependency");
    check(find(unit, "Payload") < find(unit, "Outer"), "missing by-value dependency");
    check(find(unit, "Outer") < find(unit, "access", true), "missing field dependency");
    check(find(unit, "Text") < find(unit, "lib_print"), "missing external parameter dependency");
    check(unit.declarations[find(unit, "lib_print")].type.kind == TypeKind::function,
          "external signature is not a function type");
    check(unit.entry && !unit.entry->raw && unit.entry->text == "launch", "CLI metadata lost");
    for (auto header : {"stdbool.h", "stdint.h", "stddef.h"})
        check(std::any_of(unit.dependencies.begin(), unit.dependencies.end(),
                  [&](const Dependency& dep) { return dep.header == header; }), "missing standard type header");
    check(unit.declarations[find(unit, "Max")].initializer->kind == ExprKind::opaque,
          "legacy bare constant no longer passes through");
    return unit;
}

void module_scope() {
    auto first = program("(module A (export shared) (def private_value 1) "
                         "(fn int shared () (return private_value)))").modules.front();
    auto second = program("(module B (export shared) (def private_value 2) "
                          "(fn int shared () (return private_value)))").modules.front();
    auto root = program("(library Test (fn int both () "
                        "(return (+ (call left.shared) (call right.shared)))))").modules.front();
    root.imports = {{0, "left", root.span}, {1, "right", root.span}};
    Program linked{{first, second, root}, 2};
    const auto unit = lower(linked);
    check(find(unit, "am8_m0_private_value") < find(unit, "am8_m0_shared", true),
          "missing module's private dependency");
    check(find(unit, "am8_m1_private_value") < find(unit, "am8_m1_shared", true),
          "missing module's private dependency");
    auto caller = unit.declarations[find(unit, "both", true)].body->expressions[0];
    check(caller.children[0].text == "am8_m0_shared" && caller.children[1].text == "am8_m1_shared",
          "qualified references lost module identity");

    linked.modules.back() = program("(library Test (fn int f () (return (call shared))))").modules.front();
    linked.modules.back().imports = {{0, {}, root.span}, {1, {}, root.span}};
    try {
        lower(linked);
        throw Error("expected ambiguous name");
    } catch (const Error& e) {
        check(std::string_view(e.what()).find("ambiguous name 'shared'") != std::string_view::npos, e.what());
    }
    auto& returned = linked.modules.back().forms.front().items[4].items[1];
    returned = parse("private_value", "W3.am8");
    try {
        lower(linked);
        throw Error("expected invisible private name");
    } catch (const Error& e) {
        check(std::string_view(e.what()).find("unknown name 'private_value'") != std::string_view::npos, e.what());
    }
    returned.origin_module = 0;
    const auto hygienic = lower(linked);
    check(hygienic.declarations[find(hygienic, "f", true)].body->expressions[0].text ==
          "am8_m0_private_value", "macro template lost definition-site name resolution");
}

void invalid_programs() {
    rejects("(library T (fn int f ((int x)) (return (call f))))", "expects 1 argument");
    rejects("(library T (fn int f ((int x)) (return (call f \"s\"))))", "argument type mismatch");
    rejects("(library T (fn int f () (return)))", "must return a value");
    rejects("(library T (fn void f () (return 1)))", "cannot return a value");
    rejects("(library T (fn (ptr int) f () (return 1)))", "return type mismatch");
    rejects("(library T (fn int f ((int x)) (if x (return 1))))", "may reach the end");
    rejects("(library T (fn int f () (return missing)))", "unknown name");
    rejects("(library T (fn int f () (block (block (let int x 1)) (return x))))", "unknown name");
    rejects("(library T (fn void f (((ptr (const int)) p)) (set (* p) 9)))", "not a writable lvalue");
    rejects("(library T (fn void f (((ptr (ptr int)) p)) "
            "(let (ptr (ptr (const int))) q p)))", "initializer type mismatch");
    rejects("(library T (record R ((R self))))", "contains itself by value");
    rejects("(library T (record A ((B b))) (record B ((A a))))", "cyclic declaration dependency");
    rejects("(library T (type A (ptr B)) (type B (ptr A)))", "cyclic type dependency");
    rejects("(library T (type A (array void 2)))", "array element");
    rejects("(library T (type A (array int 0)))", "positive integer");
    rejects("(library T (type A (function (array int 2) ())))", "cannot return an array");
    rejects("(library T (cpp-type H) (fn (ptr H) f () (return 0)))", "C++-only type");
    rejects("(library T (fn int f () (return 0)) (cli f))", "CLI entry must have signature");
    rejects("(library T (enum E ((A B) B)))", "not yet defined");
    rejects("(library T (var int initial (call f)) (fn int f () (return 0)))", "constant expression");
    rejects("(library T (var (array char 1) xs \"a\"))", "initializer type mismatch");
    rejects("(library T (fn int f () (return 0)) (fn int f () (return 1)))", "duplicate symbol");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto unit = typed_program();
        if (argc > 1 && std::string_view(argv[1]) == "--emit") {
            std::cout << emit(unit, Mode::api);
            return 0;
        }
        module_scope();
        invalid_programs();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
