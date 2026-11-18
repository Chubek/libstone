#include "CLI.hpp"
#include "WASM-CC.hpp"
#include "WASMDisassemble.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: wasm2c INPUT\n";
        return 1;
    }

    try {
        const auto input = limestone::wasmutils::detail::read_input(argv[1]);
        const std::vector<std::uint8_t> bytes(input.begin(), input.end());
        const auto program = limestone::wasmutils::disassemble(bytes);
        limestone::wasmutils::detail::write_output(limestone::wasmutils::emit_c(program));
    } catch (const std::exception& error) {
        std::cerr << "wasm2c: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
