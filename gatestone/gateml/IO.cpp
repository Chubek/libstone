#include "GateML.h"
#include <fstream>

namespace gateml {
std::string read_file(const std::string& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error({path}, "cannot open input file");
    std::string text;
    char buffer[8192];
    while (input) {
        input.read(buffer, sizeof(buffer)); auto n = static_cast<std::size_t>(input.gcount());
        if (n > limit - text.size()) throw Error({path}, "source size limit exceeded");
        text.append(buffer, n);
    }
    if (!input.eof()) throw Error({path}, "cannot read input file");
    return text;
}
} // namespace gateml
