#include "script_bindings.hpp"
#include <cassert>
int main() {
    moonstone::exoml::Interpreter vm;
    exoml_generated::bind_Script(vm);
    auto errors = vm.execute(moonstone::exoml::Parser("R ~get! Script:Add(19,23);").parse());
    assert(errors.empty());
    assert(std::get<std::int64_t>(vm.symbols().at("R")) == 42);
}
