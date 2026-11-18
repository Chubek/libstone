#pragma once

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace limestone::wasmutils::detail {

inline std::string read_input(const char* path) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (std::string_view(path) != "-") {
        file.open(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot open input: " + std::string(path));
        input = &file;
    }
#ifdef _WIN32
    else if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        throw std::runtime_error("cannot set stdin to binary mode");
    }
#endif
    std::string contents;
    char buffer[8192];
    do {
        input->read(buffer, sizeof(buffer));
        contents.append(buffer, static_cast<std::size_t>(input->gcount()));
    } while (*input);
    if (input->bad() || !input->eof())
        throw std::runtime_error("cannot read input: " + std::string(path));
    return contents;
}

inline void write_output(std::string_view contents) {
    if (!(std::cout << contents << std::flush))
        throw std::runtime_error("cannot write generated C");
}

} // namespace limestone::wasmutils::detail
