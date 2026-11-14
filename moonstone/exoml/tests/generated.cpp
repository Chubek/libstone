#include "bindings.hpp"
#include <cassert>
#include <limits>

using namespace moonstone::exoml;
int main() {
    Interpreter vm;
    exoml_generated::bind_Fixture(vm);
    assert(vm.services().at("Fixture:Add").info.documentation == std::string("add\0notes", 9));
    auto errors = vm.execute(Parser(R"(
A ~get! Fixture:Add(20,22);
H ~get! Fixture:exoml_half(5.0);
E ~get! Fixture:exoml_echo("hello");
L ~get! Fixture:exoml_length("hello");
W ~get! Fixture:exoml_wide(18446744073709551615u);
)")
                                 .parse());
    assert(errors.empty());
    assert(std::get<std::int64_t>(vm.symbols().at("A")) == 42);
    assert(std::get<double>(vm.symbols().at("H")) == 2.5);
    assert(std::get<std::string>(vm.symbols().at("E")) == "hello");
    assert(std::get<std::uint64_t>(vm.symbols().at("L")) == 5);
    assert(std::get<std::uint64_t>(vm.symbols().at("W")) ==
           std::numeric_limits<std::uint64_t>::max());
    assert(std::get<std::int64_t>(vm.invoke("Fixture:Minimum", {})) ==
           std::numeric_limits<std::int64_t>::min());
    assert(std::get<std::uint64_t>(vm.invoke("Fixture:Maximum", {})) ==
           std::numeric_limits<std::uint64_t>::max());
    assert(std::get<double>(vm.invoke("Fixture:Real", {})) == 1.25);
    assert(std::get<std::string>(vm.invoke("Fixture:Text", {})) ==
           std::string("\0a\n\"\\\xC3\xA9", 7));
    assert(std::get<std::vector<std::string>>(vm.invoke("Fixture:List", {})).size() == 2);
    assert(std::holds_alternative<std::monostate>(vm.invoke("Fixture:Null", {})));
    assert(!vm.execute(Parser("S ~get! Fixture:exoml_set(9);").parse()).empty());
    assert(std::get<std::int64_t>(vm.invoke("Fixture:exoml_get", {})) == 0);
    vm.enable_hatch(true);
    vm.allow_service("Fixture:exoml_set");
    assert(vm.execute(Parser("S ~hatch! Fixture:exoml_set(9);").parse()).empty());
    assert(std::get<std::int64_t>(vm.invoke("Fixture:exoml_get", {})) == 9);
    // Duplicate generated module registration is all-or-nothing.
    auto count = vm.services().size();
    bool failed = false;
    try {
        exoml_generated::bind_Fixture(vm);
    } catch (const std::invalid_argument &) {
        failed = true;
    }
    assert(failed && vm.services().size() == count);
}
