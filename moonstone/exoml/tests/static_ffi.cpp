#include <cassert>
#include <exoml/StaticFFI.hpp>

using namespace moonstone::exoml;
template <class F> void fails(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    assert(failed);
}
BindingModule fixture() {
    BindingModule m;
    m.name = "example";
    m.headers = {"example.h"};
    CFunction f;
    f.name = "add";
    f.return_type = "int";
    f.parameters = {{"a", "int"}, {"b", "int"}};
    m.functions.push_back(f);
    return m;
}
int main() {
    auto good = fixture();
    assert(validate_binding(good).empty());
    assert(generate_bindings(good).find("library.bind(\"add\", &::add, options)") !=
           std::string::npos);
    assert(generate_abi_checks(good).find("signature mismatch") != std::string::npos);
    auto invalid = [&](auto change) {
        auto m = good;
        change(m);
        assert(!binding_diagnostics(m).empty());
        fails([&] { generate_bindings(m); });
    };
    invalid([](auto &m) { m.name = "class"; });
    invalid([](auto &m) { m.headers = {"a.h\"\n#error injection"}; });
    invalid([](auto &m) { m.headers.push_back("example.h"); });
    invalid([](auto &m) { m.functions[0].return_type = "int); system(\"bad\"); //"; });
    invalid([](auto &m) { m.functions[0].parameters[0].c_type = "int[]"; });
    invalid([](auto &m) { m.functions[0].parameters[0].c_type = "void"; });
    invalid([](auto &m) { m.functions[0].parameters[0].name = "while"; });
    invalid([](auto &m) { m.functions[0].parameters[1].name = "a"; });
    invalid([](auto &m) { m.functions.push_back(m.functions.front()); });
    invalid([](auto &m) { m.constants.push_back({"add", true}); });
    invalid([](auto &m) { m.functions[0].ownership.borrowed_arguments = {2}; });
    invalid([](auto &m) {
        m.functions[0].ownership.borrowed_arguments = {0};
        m.functions[0].ownership.consumed_arguments = {0};
    });
    invalid([](auto &m) { m.functions[0].ownership.returned_borrow = true; });
    invalid([](auto &m) { m.layouts.push_back({"Record", 8, 3, {}}); });
    invalid([](auto &m) { m.layouts.push_back({"Record", 8, 8, {{"field", 9}}}); });
    assert(!binding_diagnostics(good, {.functions = 0}).empty());
    assert(!binding_diagnostics(good, {.parameters = 1}).empty());
    assert(!binding_diagnostics(good, {.text_bytes = 1}).empty());
    auto variadic = good;
    variadic.functions[0].variadic = true;
    assert(validate_binding(variadic).empty());
    fails([&] { generate_bindings(variadic); });
    assert(generate_abi_checks(variadic).find("int, int, ...") != std::string::npos);
    auto pointer = good;
    pointer.functions[0].return_type = "void *";
    assert(validate_binding(pointer).empty());
    fails([&] { generate_bindings(pointer); });
    auto consumed = good;
    consumed.functions[0].ownership.consumed_arguments = {0};
    assert(validate_binding(consumed).empty());
    fails([&] { generate_bindings(consumed); });

    StaticFFI registry;
    registry.add(good);
    fails([&] { registry.add(good); });
    Interpreter vm;
    registry.install(vm);
    assert(std::get<std::vector<std::string>>(vm.invoke("StaticFFI:Modules", {})) ==
           std::vector<std::string>{"example"});
    assert(std::get<std::string>(vm.invoke("StaticFFI:Generate", {std::string("example")})) ==
           generate_bindings(good));
    fails([&] { vm.invoke("StaticFFI:Generate", {std::string("missing")}); });

    Interpreter workspace;
    install_static_ffi(workspace);
    auto script = Parser(R"(
M ~setconst! StaticFFI:Module("math");
H ~get! StaticFFI:Header($M, "math.h");
F ~get! StaticFFI:Function($M, "add", "int", ["int", "int"]);
R ~get! StaticFFI:Rename($M, "add", "Add");
P ~get! StaticFFI:Policy($M, "add", false, false, false);
E ~get! StaticFFI:Effects($M, "add", ["read"], false);
C ~get! StaticFFI:Constant($M, "Answer", 42);
L ~get! StaticFFI:Layout($M, "struct Pair");
B ~get! StaticFFI:Field($M, "struct Pair", "first");
A ~get! StaticFFI:ExpectLayout($M, "struct Pair", 8, 4);
O ~get! StaticFFI:ExpectOffset($M, "struct Pair", "first", 0);
D ~get! StaticFFI:Describe($M);
G ~get! StaticFFI:Generate($M);
V ~get! StaticFFI:Validate($M);
)")
                      .parse();
    assert(workspace.execute(script).empty());
    auto description = std::get<std::string>(workspace.symbols().at("D"));
    assert(description.find("\"export\":\"Add\"") != std::string::npos);
    assert(std::get<std::vector<std::string>>(workspace.symbols().at("V")).empty());
    assert(std::get<std::string>(workspace.symbols().at("G"))
               .find("offsetof(struct Pair, first) == 0") != std::string::npos);
    // Invalid mutation preserves the complete old declaration.
    fails([&] {
        workspace.invoke("StaticFFI:Effects", {std::string("math"), std::string("add"),
                                               std::vector<std::string>{"unknown"}, false});
    });
    fails([&] {
        workspace.invoke("StaticFFI:Borrow",
                         {std::string("math"), std::string("add"), std::int64_t(8)});
    });
    assert(std::get<std::string>(workspace.invoke("StaticFFI:Describe", {std::string("math")})) ==
           description);
    fails([&] { install_static_ffi(workspace); });
    Interpreter collision;
    collision.register_service("Types:Name", [](const auto &) -> Value { return true; });
    fails([&] { install_static_ffi(collision); });
    assert(collision.services().size() == 1);
}
