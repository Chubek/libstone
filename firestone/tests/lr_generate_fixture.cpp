#include <liblr/DFA2Assembly.hpp>
#include <liblr/DFA2C99.hpp>
#include <liblr/LexerGenerator.hpp>

#include <cstddef>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void write_source(const char* path, const std::string& source) {
    std::ofstream file(path, std::ios::binary);
    file << source;
    file.close();
    if (!file) throw std::runtime_error(std::string("cannot write scanner: ") + path);
}
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    try {
        using namespace firestone::lr;
        const auto lexer = make_lexer({
            {"if", 10}, {"[a-z]+", 20}, {"A", 30}, {"ABCD", 40}, {R"([\x00\xff])", 50},
            {" +", 0, true}, {"@", 0}, {"Z", std::numeric_limits<std::size_t>::max()}
        });
        // Use a nonzero start state and state zero as a real transition target.
        // The four extra rows distinguish all byte columns, exercising the
        // dense emitter alongside the compressed lexer above.
        std::vector<DFAState> states(6);
        states[1].transitions['#'] = 0;
        states[0].acceptance = Acceptance{90};
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t byte = 0; byte < byte_count; ++byte)
                states[row + 2].transitions[byte] = 2 + ((byte >> (2 * row)) & 3);
        const DFA alternate(std::move(states), 1);
        write_source(argv[1], emitC99(lexer, "lr_scan_c") + "\n" + emitC99(alternate, "lr_scan_alt_c"));
        if (argc == 3)
            write_source(argv[2], emitAssembly(lexer, "lr_scan_asm") + "\n" +
                                  emitAssembly(alternate, "lr_scan_alt_asm"));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
