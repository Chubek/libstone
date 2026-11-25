#include "CLI.hpp"
#include "WASM-CC.hpp"
#include "WASM-Module.hpp"
#include "WASMDisassemble.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (argc != 2 || help) {
        (help ? std::cout : std::cerr)
            << "usage: wasm2c INPUT\n  Emit standalone C90 from a Wasm module or WASM.aml raw stream.\n";
        return help ? 0 : 2;
    }

    try {
        const auto input = limestone::wasmutils::detail::read_input(argv[1]);
        const std::vector<std::uint8_t> bytes(input.begin(), input.end());
        using namespace limestone::wasmutils;
        if (std::string_view(input).starts_with(std::string_view{"\0asm", 4}))
            detail::write_output(core::emit_module_c(core::decode_module(bytes)));
        else
            detail::write_output(emit_c(disassemble(bytes)));
    } catch (const std::exception& error) {
        std::cerr << "wasm2c: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
