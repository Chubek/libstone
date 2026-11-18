#include "CLI.hpp"
#include "WASM-CC.hpp"
#include "WATCompiler.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    using namespace limestone::wasmutils;
    if (argc != 2) {
        std::cerr << "usage: wat2c INPUT\n";
        return 2;
    }

    const auto path = std::string_view(argv[1]) == "-" ? "<stdin>" : argv[1];
    std::string input;
    try {
        input = detail::read_input(argv[1]);
        const auto program = compile_wat(input);
        detail::write_output(emit_c(program));
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
