#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
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
        throw std::runtime_error("cannot write stdout");
}

inline void write_binary(std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        throw std::runtime_error("cannot set stdout to binary mode");
#endif
    // Chunk the write instead of potentially narrowing size_t to streamsize.
    while (!bytes.empty()) {
        const auto part = bytes.first(std::min(bytes.size(), std::size_t{8192}));
        if (!std::cout.write(reinterpret_cast<const char*>(part.data()),
                             static_cast<std::streamsize>(part.size())))
            throw std::runtime_error("cannot write stdout");
        bytes = bytes.subspan(part.size());
    }
    if (!std::cout.flush())
        throw std::runtime_error("cannot flush stdout");
}

} // namespace limestone::wasmutils::detail
