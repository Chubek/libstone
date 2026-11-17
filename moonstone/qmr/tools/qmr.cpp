#include "json_import.hpp"
#include "qmr/Qmr.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

moonstone::qmr::Codec parse_codec(const std::string &name) {
    using moonstone::qmr::Codec;
    if (name == "html") return Codec::Html;
    if (name == "plain") return Codec::Plain;
    if (name == "json") return Codec::Json;
    if (name == "xml") return Codec::Xml;
    throw std::invalid_argument("unknown codec: " + name + " (html, plain, json, xml)");
}

} // namespace

int main(int argc, char **argv) {
    using namespace moonstone::qmr;
    try {
        if (argc < 2) {
            std::cerr << "Usage: qmr TEMPLATE [--root DIR] [--define KEY=VALUE]"
                         " [--data FILE.json] [--codec html|plain|json|xml]"
                         " [--no-escape] [--max-output BYTES] [--check]\n";
            return 2;
        }
        const fs::path file = argv[1];
        Context context;
        Options options;
        fs::path root;
        fs::path data_file;
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
            } else if (arg == "--data" && i + 1 < argc) {
                data_file = argv[++i];
            } else if (arg == "--codec" && i + 1 < argc) {
                options.codec = parse_codec(argv[++i]);
            } else if (arg == "--no-escape") options.codec = Codec::Plain;
            else if (arg == "--max-output" && i + 1 < argc) {
                const auto n = std::stoull(argv[++i]);
                if (n == 0 || n > 1024ULL * 1024ULL * 1024ULL)
                    throw std::invalid_argument("--max-output range: 1..1073741824");
                options.limits.max_output_bytes = static_cast<std::size_t>(n);
            } else if (arg == "--check") check_only = true;
            else throw std::invalid_argument("unknown or incomplete option: " + arg);
        }
        if (!data_file.empty()) {
            // Strict, contract-validated JSON import; --define entries win.
            Context loaded = cli::load_json_context(data_file, options.limits);
            for (auto &entry : context) loaded[entry.first] = std::move(entry.second);
            context = std::move(loaded);
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
