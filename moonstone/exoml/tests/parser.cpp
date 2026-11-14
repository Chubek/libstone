#include <cassert>
#include <exoml/StaticFFI.hpp>
#include <future>

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
int main() {
    const std::string source =
        "-- header\n  M ~setconst! Types:String(42); -- trailing\nN ~get! Types:String($@);\n";
    auto module = Parser(source, "example.exo").parse();
    assert(module.commands.size() == 2);
    assert(module.commands[0].source.file == "example.exo");
    assert(module.commands[0].source.line == 2 && module.commands[0].source.column == 3);
    assert(module.commands[0].source.end_line == 2 && module.commands[0].source.end_column == 33);
    assert(module.commands[1].source.line == 3 && module.commands[1].source.end_column == 26);
    assert(Parser("-- comment only").parse().commands.empty());
    assert(Parser("M ~get! Types:Join([  ], \"\");").parse().commands[0].arguments[0] == "[  ]");
    try {
        Parser("\nM ~get! A(1,);", "bad.exo").parse();
        assert(false);
    } catch (const ParseError &error) {
        assert(error.source.file == "bad.exo" && error.source.line == 2 &&
               error.source.column == 13);
    }
    Interpreter vm;
    install_static_ffi(vm);
    assert(vm.execute(module).empty());
    assert(std::get<std::string>(vm.symbols().at("N")) == "42");
    auto snap = vm.snapshot();
    vm.clear_symbols();
    assert(snap.size() == 2 && vm.symbols().empty());
    assert(!vm.execute(Parser("M ~get! Types:String($@);").parse()).empty());
    assert(
        vm.execute(
              Parser(
                  R"(S ~get! Types:String("x\n\t\"\\\0"); U ~get! Types:Name(18446744073709551615u); R ~get! Types:Name(-1.25e2); L ~get! Types:Join(["a", "b"], ","); Z ~get! Types:IsNull(null);)")
                  .parse())
            .empty());
    assert(std::get<std::string>(vm.symbols().at("U")) == "unsigned");
    assert(std::get<std::string>(vm.symbols().at("R")) == "real");
    assert(std::get<std::string>(vm.symbols().at("L")) == "a,b");
    assert(std::get<bool>(vm.symbols().at("Z")));
    for (const auto *bad :
         {"M ~unknown! A();", "M ~get! A(1,);", "M ~get! A(1)", "9M ~get! A();", "M ~get! A::B();",
          "M ~get! A(9223372036854775808);", "M ~get! A(18446744073709551616u);",
          "M ~get! A(1.5u);", "M ~get! A(1e999);", "M ~get! A(\"bad\\q\");", "M ~get! A([1]);",
          "M ~get! A(truefalse);"})
        fails([&] { Parser(bad).parse(); });
    fails([&] { Parser(std::string_view("a\0b", 3)).parse(); });
    fails([&] { Parser(source, {}, {.input_bytes = 10}).parse(); });
    fails([&] { Parser(source, {}, {.commands = 1}).parse(); });
    fails([&] { Parser("M ~get! A(1,2);", {}, {.arguments = 1}).parse(); });
    // Caller-built ASTs must not bypass syntax/operator validation.
    int calls = 0;
    vm.register_service("Host:Call", [&](const auto &) -> Value {
        ++calls;
        return true;
    });
    Module forged;
    Command command;
    command.binding = "Valid";
    command.operation = "~unknown!";
    command.function = "Host:Call";
    forged.commands.push_back(command);
    assert(vm.execute(forged).size() == 1 && calls == 0);
    forged.commands[0].operation = "~get!";
    forged.commands[0].binding = "bad:name";
    assert(vm.execute(forged).size() == 1 && calls == 0);
    forged.commands[0].binding = "Good";
    assert(vm.execute(forged, {.max_commands = 0}).size() == 1 && calls == 0);
    auto stop = Parser("bad ~get! Missing(); next ~get! Host:Call();").parse();
    assert(vm.execute(stop, {.stop_on_error = true}).size() == 1 && calls == 0);
    assert(vm.execute(stop).size() == 1 && calls == 1);
    // Generator state is serialized across independent parser instances.
    auto parse_many = [&] {
        for (int i = 0; i < 50; ++i)
            assert(Parser(source).parse().commands.size() == 2);
    };
    auto other = std::async(std::launch::async, parse_many);
    parse_many();
    other.get();
    // Long lists/statements use iterative generated left recursion.
    std::string many;
    for (int i = 0; i < 1000; ++i)
        many += "X ~get! Types:Name(0);\n";
    assert(Parser(many).parse().commands.size() == 1000);
}
