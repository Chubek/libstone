#include <gateml/GateML.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
const char* help = R"(GateML frontend compiler
Usage:
  gateml-cli check <file.gml|bundle-directory>
  gateml-cli compile <file.gml|bundle-directory>
  gateml-cli grammar <file.gml|bundle-directory>
  gateml-cli parse <file.gml|bundle-directory> <source-file>
  gateml-cli lower <file.gml|bundle-directory> <source-file>
  gateml-cli --help

Examples:
  gateml-cli check gatestone/gatebindle/C
  gateml-cli lower gatestone/gatebindle/C expression.c

compile emits a versioned frontend JSON descriptor;
grammar emits NovoParse BNF over token codes; parse emits source-tree JSON;
lower emits Jiterati IR. Outputs go to stdout.
Exit codes: 0 success; 1 input/compilation error; 2 invalid arguments.
)";
std::string read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string("cannot open ") + path);
    std::ostringstream text; text << input.rdbuf();
    if (input.bad()) throw std::runtime_error(std::string("cannot read ") + path);
    return text.str();
}
}
int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "help")) {
        std::cout << help; return 0;
    }
    if (argc >= 3 && std::string(argv[2]) == "--help") { std::cout << help; return 0; }
    std::string command = argc > 1 ? argv[1] : "";
    bool source_command = command == "parse" || command == "lower";
    if ((!source_command && command != "check" && command != "compile" && command != "grammar") || argc != (source_command ? 4 : 3)) {
        std::cerr << help; return 2;
    }
    std::string diagnostic_path = argv[2];
    try {
        auto frontend = gateml::load(argv[2]);
        if (command == "check") std::cout << frontend.name << ": " << frontend.lexer.size() << " lexical rules, "
            << frontend.productions.size() << " productions\n";
        else if (command == "grammar") std::cout << gateml::grammar(frontend);
        else if (command == "compile") std::cout << gateml::descriptor(frontend) << '\n';
        else {
            diagnostic_path = argv[3];
            auto tree = gateml::parse(frontend, read(argv[3]));
            if (command == "parse") std::cout << gateml::json(tree) << '\n';
            else std::cout << gateml::lower(frontend, tree)->to_string();
        }
        return 0;
    } catch (const gateml::Error& error) {
        std::cerr << (error.source.empty() ? diagnostic_path : error.source) << ':' << error.span.line << ':'
            << error.span.column << ": error: " << error.what() << '\n';
    } catch (const std::exception& error) { std::cerr << "error: " << error.what() << '\n'; }
    return 1;
}
