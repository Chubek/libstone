#include "bootstrap.hpp"

#include <iostream>

int main(int argc, char** argv) {
    domios::Bootstrap bootstrap;
    if (argc == 3 && std::string(argv[1]) == "--emit-dit-template") {
        std::string error;
        if (!bootstrap.emit_dit_template(argv[2], error)) {
            std::cerr << error << '\n';
            return 1;
        }
        return 0;
    }
    std::string error;
    if (!bootstrap.run(error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}

