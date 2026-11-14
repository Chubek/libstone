#include <exoml/StaticFFI.hpp>
int main() {
    moonstone::exoml::Interpreter vm;
    moonstone::exoml::bind_native(vm, "Math:Add", [](int a, int b) { return a + b; });
    vm.enable_hatch(true);
    vm.allow_service("Math:Add");
    auto errors = vm.execute(moonstone::exoml::Parser("X ~hatch! Math:Add(19,23);").parse());
    return !errors.empty() || std::get<std::int64_t>(vm.symbols().at("X")) != 42;
}
