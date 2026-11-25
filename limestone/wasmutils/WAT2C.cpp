#include "CLI.hpp"
#include "WASM-CC.hpp"
#include "WAT-Module.hpp"
#include "WATCompiler.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    using namespace limestone::wasmutils;
    const bool module = argc == 3 && std::string_view(argv[1]) == "--module";
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (help || (argc != 2 && !module)) {
        (help ? std::cout : std::cerr)
            << "usage: wat2c [--module] INPUT\n"
               "  Emit standalone C90; --module selects Core Wasm text instead of WASM.aml syntax.\n";
        return help ? 0 : 2;
    }

    const auto path = std::string_view(argv[argc - 1]) == "-" ? "<stdin>" : argv[argc - 1];
    std::string input;
    try {
        input = detail::read_input(argv[argc - 1]);
        if (module)
            detail::write_output(core::emit_module_c(core::assemble_module(input)));
        else
            detail::write_output(emit_c(compile_wat(input)));
    } catch (Error const& error) {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < input.size() && i < error.offset; ++i) {
            if (input[i] == '\r') {
                ++line;
                column = 1;
            } else if (input[i] == '\n') {
                if (i == 0 || input[i - 1] != '\r')
                    ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        std::cerr << path << ':' << line << ':' << column << ": " << error.what() << '\n';
        return 1;
    } catch (std::exception const& error) {
        std::cerr << "wat2c: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
