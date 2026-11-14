#include <cassert>
#include <exoml/StaticFFI.hpp>
#include <future>
#include <limits>

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
int add(int a, int b) noexcept { return a + b; }
enum class Code : unsigned char { Okay = 7 };
int main() {
    Interpreter vm;
    bind_native(vm, "C:Add", &add);
    fails([&] { vm.invoke("C:Add", {std::int64_t(2), std::int64_t(3)}); });
    vm.enable_hatch(true);
    vm.allow_service("C:Add");
    assert(std::get<std::int64_t>(
               vm.invoke("C:Add", {std::int64_t(2), std::int64_t(3)}, "~hatch!")) == 5);
    vm.revoke_service("C:Add");
    fails([&] { vm.invoke("C:Add", {std::int64_t(2), std::int64_t(3)}, "~hatch!"); });
    assert(vm.services().at("C:Add").info.arity == 2);
    assert(vm.services().at("C:Add").info.parameter_types ==
           std::vector<std::string>({"int", "int"}));
    assert(vm.services().at("C:Add").info.ownership.effects.unknown);

    NativeOptions public_call;
    public_call.privileged = false;
    int calls = 0;
    bind_native(
        vm, "C:Byte",
        [&calls](unsigned char x) {
            ++calls;
            return x;
        },
        public_call);
    for (Value invalid : {Value{std::int64_t(-1)}, Value{std::uint64_t(256)}, Value{true},
                          Value{2.0}, Value{std::string("3")}})
        fails([&] { vm.invoke("C:Byte", {invalid}); });
    fails([&] { vm.invoke("C:Byte", {}); });
    fails([&] { vm.invoke("C:Byte", {std::int64_t(1), std::int64_t(2)}); });
    assert(calls == 0);
    assert(std::get<std::uint64_t>(vm.invoke("C:Byte", {std::int64_t(255)})) == 255);
    bind_native(vm, "C:Signed", [](std::int64_t x) { return x; }, public_call);
    auto minimum = std::numeric_limits<std::int64_t>::min();
    assert(std::get<std::int64_t>(vm.invoke("C:Signed", {minimum})) == minimum);
    fails([&] { vm.invoke("C:Signed", {std::numeric_limits<std::uint64_t>::max()}); });
    bind_native(vm, "C:Unsigned", [](std::uint64_t x) { return x; }, public_call);
    assert(std::get<std::uint64_t>(
               vm.invoke("C:Unsigned", {std::numeric_limits<std::uint64_t>::max()})) ==
           std::numeric_limits<std::uint64_t>::max());
    bind_native(vm, "C:Real", [](float x) { return x * 0.5f; }, public_call);
    assert(std::get<double>(vm.invoke("C:Real", {4.0})) == 2.0);
    assert(std::get<double>(vm.invoke("C:Real", {std::int64_t(4)})) == 2.0);
    fails([&] { vm.invoke("C:Real", {std::numeric_limits<double>::infinity()}); });
    fails([&] { vm.invoke("C:Real", {std::numeric_limits<double>::max()}); });
    fails([&] { vm.invoke("C:Real", {std::uint64_t(9007199254740993ULL)}); });
    bind_native(vm, "C:Bool", [](bool x) { return !x; }, public_call);
    assert(!std::get<bool>(vm.invoke("C:Bool", {true})));
    fails([&] { vm.invoke("C:Bool", {std::int64_t(1)}); });
    bind_native(vm, "C:Enum", [](Code x) { return x; }, public_call);
    assert(std::get<std::uint64_t>(vm.invoke("C:Enum", {std::int64_t(7)})) == 7);

    bind_native(
        vm, "C:String", [](const std::string &a, std::string_view b) { return a + std::string(b); },
        public_call);
    assert(std::get<std::string>(vm.invoke(
               "C:String", {std::string("a\0", 2), std::string("b")})) == std::string("a\0b", 3));
    auto echo = [](const char *text) -> const char * { return text; };
    fails([&] { bind_native(vm, "C:CString", echo, public_call); });
    auto strings = public_call;
    strings.c_strings = true;
    bind_native(vm, "C:CString", echo, strings);
    assert(std::get<std::string>(vm.invoke("C:CString", {std::string("copied")})) == "copied");
    fails([&] { vm.invoke("C:CString", {std::string("x\0y", 3)}); });
    strings.max_string_bytes = 2;
    bind_native(vm, "C:ShortString", echo, strings);
    fails([&] { vm.invoke("C:ShortString", {std::string("abc")}); });
    bind_native(vm, "C:Null", []() -> const char * { return nullptr; }, strings);
    assert(std::holds_alternative<std::monostate>(vm.invoke("C:Null", {})));
    bind_native(vm, "C:Optional", [](std::optional<int> x) { return x; }, public_call);
    assert(std::holds_alternative<std::monostate>(vm.invoke("C:Optional", {std::monostate{}})));
    assert(std::get<std::int64_t>(vm.invoke("C:Optional", {std::int64_t(9)})) == 9);
    bind_native(vm, "C:Void", [&calls] { ++calls; }, public_call);
    assert(std::holds_alternative<std::monostate>(vm.invoke("C:Void", {})));
    bind_native(
        vm, "C:Throw", []() -> int { throw std::runtime_error("original cause"); }, public_call);
    auto diagnostics = vm.execute(Parser("bad ~get! C:Throw(); good ~get! C:Byte(8);").parse());
    assert(diagnostics.size() == 1 && diagnostics[0].message == "original cause");
    assert(!vm.symbols().count("bad") && vm.symbols().count("good"));
    bind_native(vm, "C:ThrowOther", []() -> int { throw 1; }, public_call);
    assert(vm.execute(Parser("bad ~get! C:ThrowOther();").parse())[0].id == "EXOML-NATIVE");

    auto affine = public_call;
    affine.thread_affine = true;
    bind_native(vm, "C:Affine", [] { return 1; }, affine);
    std::async(std::launch::async, [&] { fails([&] { vm.invoke("C:Affine", {}); }); }).get();
    assert(std::get<std::int64_t>(vm.invoke("C:Affine", {})) == 1);
    auto invalid = public_call;
    invalid.ownership.may_escape = true;
    fails([&] { bind_native(vm, "C:Escape", echo, invalid); });
    invalid = public_call;
    invalid.ownership.borrowed_arguments.insert(9);
    fails([&] { bind_native(vm, "C:Borrow", &add, invalid); });
    invalid = public_call;
    invalid.ownership.effects.effects.insert(static_cast<moonstone::Effect>(999));
    fails([&] { bind_native(vm, "C:Effect", &add, invalid); });
    int (*null_function)(int, int) = nullptr;
    fails([&] { bind_native(vm, "C:NullFunction", null_function); });

    StaticLibrary library("Math");
    library.bind("Add", &add, public_call).alias("Sum", "Add").constant("Answer", std::int64_t(42));
    AutoBindOptions selected;
    selected.name_space = "App:Math";
    selected.include = {"Sum", "Answer"};
    selected.rename = {{"Sum", "Plus"}};
    library.install(vm, selected);
    assert(vm.services().count("App:Math:Plus") && !vm.services().count("App:Math:Add"));
    assert(std::get<std::int64_t>(vm.invoke("App:Math:Answer", {})) == 42);
    auto before = vm.services().size();
    fails([&] { library.install(vm, selected); });
    assert(vm.services().size() == before);
    selected.include.insert("Missing");
    fails([&] { library.install(vm, selected); });
    selected = {};
    selected.rename = {{"Add", "Same"}, {"Sum", "Same"}};
    fails([&] { library.install(vm, selected); });
    assert(vm.services().size() == before);
    Interpreter::Services batch;
    batch.emplace("A:New", native_service([] { return 1; }, public_call));
    batch.emplace("C:Add", native_service(&add));
    fails([&] { vm.register_services(batch); });
    assert(!vm.services().count("A:New"));
}
