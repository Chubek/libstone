#include "CLI.hpp"
#include "WASM-Validate.hpp"

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;

int main(int argc, char** argv) {
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (argc != 2 || help) {
        (help ? std::cout : std::cerr) << "usage: wasm-validate INPUT\n";
        return help ? 0 : 2;
    }
    try {
        const auto input = wasm::detail::read_input(argv[1]);
        const std::vector<std::uint8_t> bytes(input.begin(), input.end());
        core::validate_module(core::decode_module(bytes));
    } catch (const std::exception& error) {
        std::cerr << "wasm-validate: " << argv[1] << ": " << error.what() << '\n';
        return 1;
    }
}
