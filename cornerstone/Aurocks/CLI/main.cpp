#include "aurocks/Compiler.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void usage(std::ostream& out, const char* program) {
    out << "Usage: " << program << " [options] grammar.g\n"
        << "Generate a scannerless C99 parser (standard output by default).\n\n"
        << "  -e, --entrypoint NAME   Add a named parser entry point\n"
        << "  -o, --output FILE       Write C source to FILE ('-' for standard output)\n"
        << "      --check             Check and report analysis without writing C source\n"
        << "      --backend NAME      Select the backend\n"
        << "      --algorithm NAME    Select the parser analysis algorithm\n"
        << "      --disambiguator NAME  Select the disambiguation policy\n"
        << "      --lua FILE          Supply Lua extension source explicitly\n"
        << "      --no-strpool        Compatibility option; no pool is emitted\n"
        << "  -V, --version           Show the generator version\n"
        << "  -h, --help              Show this help\n";
}

bool readFile(const std::string& path, std::string& text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << path << ": error: cannot open input file\n";
        return false;
    }
    std::ostringstream contents;
    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)))
        contents.write(buffer, input.gcount());
    contents.write(buffer, input.gcount());
    if (!input.eof() || input.bad() || !contents) {
        std::cerr << path << ": error: cannot read input file\n";
        return false;
    }
    text = contents.str();
    return true;
}

bool writeSource(const std::string& path, const std::string& text) {
    if (path.empty() || path == "-") {
        std::cout << text << std::flush;
        if (std::cout)
            return true;
        std::cerr << "stdout: error: cannot write C source\n";
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << path << ": error: cannot open output file\n";
        return false;
    }
    output << text;
    output.close();
    if (!output) {
        std::cerr << path << ": error: cannot write C source\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    aurocks::Options options;
    std::string grammarPath;
    std::string outputPath;
    std::string luaPath;
    bool positionalOnly = false;
    auto usageError = [&](std::string_view message) {
        std::cerr << argv[0] << ": " << message << "\n"
                  << "Use --help for usage.\n";
        return 2;
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (!positionalOnly && arg == "--") {
            positionalOnly = true;
            continue;
        }
        if (!positionalOnly && !arg.empty() && arg.front() == '-') {
            const auto equals = arg.find('=');
            const auto name = arg.substr(0, equals);
            if (arg == "--help" || arg == "-h") {
                usage(std::cout, argv[0]);
                return 0;
            }
            if (arg == "--version" || arg == "-V") {
                std::cout << "aurocks 2.0 (native)\n";
                return 0;
            }
            if (arg == "--check") {
                options.check_only = true;
                continue;
            }
            if (arg == "--no-strpool")
                continue;
            if (name == "--strpool" || name == "--poolfile")
                return usageError(std::string(name) +
                                  " is unsupported: the native generator does not emit string pools; "
                                  "remove this option or use --no-strpool");
            if (name == "--liblr-helper")
                return usageError("--liblr-helper is unsupported: remove this option to use built-in matchers; "
                                  "--backend liblr selects grammar analysis, not regex DFA generation");
            std::string* value = nullptr;
            if (name == "-e" || name == "--entrypoint")
                value = &options.entrypoint;
            else if (name == "-o" || name == "--output")
                value = &outputPath;
            else if (name == "--backend")
                value = &options.backend;
            else if (name == "--algorithm")
                value = &options.algorithm;
            else if (name == "--disambiguator")
                value = &options.disambiguator;
            else if (name == "--lua")
                value = &luaPath;
            else
                return usageError("unknown option '" + std::string(arg) + "'");

            if (equals != std::string_view::npos) {
                *value = arg.substr(equals + 1);
            } else if (i + 1 < argc &&
                       (std::string_view(argv[i + 1]) == "-" || argv[i + 1][0] != '-')) {
                *value = argv[++i];
            } else {
                return usageError(std::string(name) + " requires an argument");
            }
            if (value->empty())
                return usageError(std::string(name) + " requires a nonempty argument");
            continue;
        }
        if (arg.empty() || !grammarPath.empty())
            return usageError("expected exactly one grammar input file");
        grammarPath = arg;
    }
    if (grammarPath.empty())
        return usageError("expected a grammar input file");

    std::string grammar;
    if (!readFile(grammarPath, grammar))
        return 1;
    if (!luaPath.empty() && !readFile(luaPath, options.lua_source.emplace()))
        return 1;

    const auto result = aurocks::compile(grammar, options);
    for (const auto& diagnostic : result.diagnostics) {
        std::cerr << grammarPath << ':' << diagnostic.location.line << ':'
                  << diagnostic.location.column << ": "
                  << (diagnostic.severity == aurocks::Severity::error ? "error" : "warning");
        if (!diagnostic.code.empty())
            std::cerr << " [" << diagnostic.code << ']';
        std::cerr << ": " << diagnostic.message << '\n';
    }
    if (!result.ok())
        return 1;
    if (options.check_only) {
        const auto& analysis = result.analysis;
        std::cout << "backend=" << analysis.backend << " algorithm=" << analysis.algorithm
                  << " disambiguator=" << analysis.disambiguator << " states=" << analysis.states
                  << " conflicts=" << analysis.conflicts << '\n' << std::flush;
        return std::cout ? 0 : 1;
    }
    return writeSource(outputPath, result.source) ? 0 : 1;
}
