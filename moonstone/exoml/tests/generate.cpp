#include "fixture.h"
#include <exoml/StaticFFI.hpp>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace moonstone::exoml;
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    BindingModule module;
    module.name = "Fixture";
    module.headers = {"fixture.h"};
    auto function = [&](std::string name, std::string result, std::vector<CParameter> parameters,
                        bool strings = false) {
        CFunction f;
        f.name = std::move(name);
        f.return_type = std::move(result);
        f.parameters = std::move(parameters);
        f.privileged = false;
        f.c_strings = strings;
        f.ownership.effects.unknown = true;
        module.functions.push_back(std::move(f));
    };
    function("exoml_add", "int", {{"a", "int"}, {"b", "int"}});
    module.functions.back().exported_name = "Add";
    module.functions.back().documentation = std::string("add\0notes", 9);
    function("exoml_half", "double", {{"value", "double"}});
    function("exoml_echo", "const char *", {{"value", "const char *"}}, true);
    function("exoml_length", "size_t", {{"value", "const char *"}}, true);
    function("exoml_wide", "unsigned long long", {{"value", "unsigned long long"}});
    function("exoml_set", "void", {{"value", "int"}});
    module.functions.back().privileged = true;
    function("exoml_get", "int", {});
    module.constants = {{"Minimum", std::numeric_limits<std::int64_t>::min()},
                        {"Maximum", std::numeric_limits<std::uint64_t>::max()},
                        {"Real", 1.25},
                        {"Text", std::string("\0a\n\"\\\xC3\xA9", 7)},
                        {"List", std::vector<std::string>{"a", "b"}},
                        {"Null", std::monostate{}}};
    module.layouts.push_back(
        {"struct exoml_pair",
         sizeof(exoml_pair),
         alignof(exoml_pair),
         {{"tag", offsetof(exoml_pair, tag)}, {"value", offsetof(exoml_pair, value)}}});
    auto write = [&](const char *name, const std::string &contents) {
        std::ofstream output(std::filesystem::path(argv[1]) / name, std::ios::binary);
        if (!(output << contents))
            throw std::runtime_error("cannot write generated fixture");
    };
    write("bindings.hpp", generate_bindings(module));
    write("checks.cpp", generate_abi_checks(module));
    write("probe.cpp", generate_abi_probe(module));
    write("manifest.json", describe_binding(module));
    module.functions[0].return_type = "double";
    write("bad_signature.cpp", generate_abi_checks(module));
    module.functions[0].return_type = "int";
    module.layouts[0].size = sizeof(exoml_pair) + 1;
    write("bad_layout.cpp", generate_abi_checks(module));
}
