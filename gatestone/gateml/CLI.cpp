#include "GateML.h"
#include <charconv>
#include <fstream>
#include <iostream>

namespace gateml {
namespace {
void usage(std::ostream& out) {
    out << "GateML 0.1 - strict ML on MetaChain\n"
           "usage: gateml-cli [run|check|type|ir|build] FILE [--no-stdlib] [-o FILE]\n"
           "       gateml-cli [run|check|type|ir|build] -e SOURCE [--no-stdlib]\n"
           "       gateml-cli stdlib\n"
           "options: --max-steps N, --help, --version\n";
}
}
int cli(int argc, char** argv) {
    try {
        if (argc == 1) { usage(std::cerr); return 2; }
        std::string command = argv[1];
        if (command == "--help" || command == "-h") { usage(std::cout); return 0; }
        if (command == "--version") { std::cout << "GateML 0.1.0\n"; return 0; }
        if (command == "stdlib" && argc == 2) { std::cout << standard_library(); return 0; }
        if (command != "run" && command != "check" && command != "type" && command != "ir" && command != "build") {
            usage(std::cerr); return 2;
        }
        Options options; std::string path, source, output; bool supplied = false, inline_source = false;
        for (int n = 2; n < argc; ++n) {
            std::string arg = argv[n];
            if (arg == "--no-stdlib") options.standard_library = false;
            else if (arg == "-e" || arg == "-o" || arg == "--max-steps") {
                if (++n >= argc) { std::cerr << "missing value for " << arg << '\n'; return 2; }
                if (arg == "-e") {
                    if (supplied) { std::cerr << "only one input is allowed\n"; return 2; }
                    supplied = inline_source = true; source = argv[n]; path = "<command>";
                } else if (arg == "-o") output = argv[n];
                else {
                    std::string_view text = argv[n];
                    auto r = std::from_chars(text.data(), text.data() + text.size(), options.max_steps);
                    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || options.max_steps == 0) {
                        std::cerr << "--max-steps requires a positive integer\n"; return 2;
                    }
                }
            } else if (!arg.empty() && arg[0] == '-') { std::cerr << "unknown option: " << arg << '\n'; return 2; }
            else {
                if (supplied) { std::cerr << "only one input is allowed\n"; return 2; }
                supplied = true; path = arg;
            }
        }
        if (!supplied || (command == "build" && output.empty()) || (command != "build" && !output.empty())) {
            usage(std::cerr); return 2;
        }
        if (!inline_source) source = read_file(path, options.max_source_bytes);
        if (command == "build") {
            auto bytes = executable(source, options);
            std::ofstream file(output, std::ios::binary);
            if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw Error({output}, "cannot write executable");
            file.close(); if (!file) throw Error({output}, "cannot finish executable");
        } else {
            auto program = compile(source, path, options);
            if (command == "check") std::cout << "ok\n";
            else if (command == "type") std::cout << program.type << '\n';
            else if (command == "ir") std::cout << disassemble(program);
            else { auto result = run(program, options); std::cout << result.output << result.value.show() << '\n'; }
        }
        return 0;
    } catch (const Error& e) {
        std::cerr << e.location.file << ':' << e.location.line << ':' << e.location.column << ": " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) { std::cerr << "gateml: " << e.what() << '\n'; return 1; }
}
} // namespace gateml
