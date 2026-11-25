#include "CLI.hpp"
#include "WASM-Validate.hpp"
#include "WAT-Module.hpp"

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;

int main(int argc, char** argv) {
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (argc != 2 || help) {
        (help ? std::cout : std::cerr) << "usage: wasm-assemble INPUT\n";
        return help ? 0 : 2;
    }
    std::string input;
    try {
        input = wasm::detail::read_input(argv[1]);
        const auto module = core::assemble_module(input);
        core::validate_module(module);
        wasm::detail::write_binary(core::encode_module(module));
    } catch (const wasm::Error& error) {
        const auto path = std::string_view(argv[1]) == "-" ? "<stdin>" : argv[1];
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < input.size() && i < error.offset; ++i) {
            if (input[i] == '\r' || (input[i] == '\n' && (i == 0 || input[i - 1] != '\r'))) {
                ++line;
                column = 1;
            } else if (input[i] != '\n') {
                ++column;
            }
        }
        std::cerr << path << ':' << line << ':' << column << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "wasm-assemble: " << error.what() << '\n';
        return 1;
    }
}
