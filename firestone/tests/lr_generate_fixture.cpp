#include <liblr/DFA2Assembly.hpp>
#include <liblr/DFA2C99.hpp>
#include <liblr/LexerGenerator.hpp>

#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

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
        write_source(argv[1], emitC99(lexer, "lr_scan_c"));
        if (argc == 3) write_source(argv[2], emitAssembly(lexer, "lr_scan_asm"));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
