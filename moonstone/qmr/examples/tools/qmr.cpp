#include "qmr/Qmr.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
    using namespace moonstone::qmr;
    try {
        if (argc < 2) {
            std::cerr << "Usage: qmr TEMPLATE [--root DIR] [--define KEY=VALUE]"
                         " [--no-escape] [--max-output BYTES] [--check]\n";
            return 2;
        }
        const fs::path file = argv[1];
        Context context;
        Options options;
        fs::path root;
        bool check_only = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--root" && i + 1 < argc) root = argv[++i];
            else if (arg == "--define" && i + 1 < argc) {
                const std::string definition = argv[++i];
                const auto eq = definition.find('=');
                if (eq == std::string::npos || eq == 0)
                    throw std::invalid_argument("--define requires KEY=VALUE");
                context[definition.substr(0, eq)] = definition.substr(eq + 1);
            } else if (arg == "--no-escape") options.escape = EscapeMode::None;
            else if (arg == "--check") check_only = true;
            else if (arg == "--max-output" && i + 1 < argc) {
                const auto n = std::stoull(argv[++i]);
                if (n == 0 || n > 1024ULL * 1024ULL * 1024ULL)
                    throw std::invalid_argument("--max-output range: 1..1073741824");
                options.limits.max_output_bytes = static_cast<std::size_t>(n);
            } else throw std::invalid_argument("unknown or incomplete option: " + arg);
        }
        if (check_only) {
            // --check also exercises the Lua compiler for a full syntax check.
            Engine(std::move(options)).render_file(file, context, root);
            std::cout << "OK\n";
        } else std::cout << Engine(std::move(options)).render_file(file, context, root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "qmr: " << e.what() << '\n';
        return 1;
    }
}
