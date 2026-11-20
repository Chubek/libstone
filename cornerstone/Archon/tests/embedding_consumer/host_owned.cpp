#include <Python.h>
#include <archon/Archon.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    Py_Initialize();
    int result = 0;
    try {
        archon::add_package_path(argv[1]);
        const auto ast = archon::parse_source("architecture Attached {}");
        if (ast.at("name").as_string() != "Attached" || !PyGILState_Check())
            throw std::runtime_error("facade did not leave the caller's GIL state intact");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    return Py_FinalizeEx() == 0 ? result : 1;
}
