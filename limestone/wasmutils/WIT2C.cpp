#include "CLI.hpp"
#include "WIT-Canonical.hpp"
#include "WITParser.hpp"

namespace {

void usage(std::ostream& out) {
    out << "usage: wit2c [--canonical] [--dep PATH]... INPUT\n"
           "Emit ANSI C host declarations by default.\n"
           "--canonical reports synchronous memory32/UTF-8 Canonical ABI signatures and layouts.\n"
           "Resource and async ABIs, C adapter generation, and component binary linking are not supported.\n"
           "Supply WIT dependency packages explicitly with --dep. Use '-' to read from stdin.\n";
}

} // namespace

int main(int argc, char** argv) {
    using namespace limestone::wasmutils;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    std::vector<const char*> dependency_paths;
    const char* input = nullptr;
    bool options = true;
    bool canonical_mode = false;
    unsigned stdin_count = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (options && argument == "--") {
            options = false;
        } else if (options && argument == "--canonical") {
            canonical_mode = true;
        } else if (options && argument == "--dep") {
            if (++i == argc) {
                usage(std::cerr);
                return 2;
            }
            dependency_paths.push_back(argv[i]);
            stdin_count += std::string_view(argv[i]) == "-";
        } else if (input) {
            usage(std::cerr);
            return 2;
        } else {
            input = argv[i];
            stdin_count += argument == "-";
        }
    }
    if (!input || stdin_count > 1) {
        usage(std::cerr);
        return 2;
    }

    const char* source = input;
    try {
        if (!canonical_mode && dependency_paths.empty()) {
            detail::write_output(wit_to_c(detail::read_input(input)));
            return 0;
        }
        const auto document = parse_wit(detail::read_input(input));
        std::vector<WitDocument> dependencies;
        dependencies.reserve(dependency_paths.size());
        for (const auto* path : dependency_paths) {
            source = path;
            dependencies.push_back(parse_wit(detail::read_input(path)));
        }
        source = input;
        detail::write_output(canonical_mode ? canonical::describe(document, dependencies) :
                                              emit_wit_c(document, dependencies));
    } catch (const WitError& error) {
        if (error.dependency_index)
            source = dependency_paths.at(*error.dependency_index);
        std::cerr << source << ':' << error.location.line << ':'
                  << error.location.column << ": error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "wit2c: " << error.what() << '\n';
        return 1;
    }
}
