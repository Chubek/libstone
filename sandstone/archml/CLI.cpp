#include "Target.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
void usage(std::ostream &out) {
    out << "Usage: archml [options] INPUT\n"
        << "  --check                 Validate a profile\n"
        << "  --target cpp|json|archml Select generated output (default: cpp)\n"
        << "  --namespace NAME        C++ namespace (default: archml_<profile>)\n"
        << "  -o, --output PATH       Write output (default: stdout)\n"
        << "  --list-targets          List generation backends\n"
        << "  --help, --version       Show help or version\n"
        << "INPUT or output '-' means stdin or stdout.\n";
}
}
int main(int argc, char **argv) {
    try {
        std::string input, output = "-", target = "cpp";
        sandstone::archml::TargetOptions options;
        bool check = false, positional = false, explicit_target = false, explicit_output = false;
        auto value = [&](int &i) -> std::string {
            if (++i == argc) throw std::invalid_argument(std::string("missing value for ") + argv[i - 1]);
            return argv[i];
        };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (!positional && arg == "--") { positional = true; continue; }
            if (!positional && (arg == "--help" || arg == "-h")) { usage(std::cout); return 0; }
            if (!positional && arg == "--version") { std::cout << "Sandstone ArchML 1.0\n"; return 0; }
            if (!positional && arg == "--list-targets") {
                for (auto name : sandstone::archml::target_names()) std::cout << name << '\n';
                return 0;
            }
            if (!positional && arg == "--check") check = true;
            else if (!positional && arg == "--target") { target = value(i); explicit_target = true; }
            else if (!positional && arg == "--namespace") options.namespace_name = value(i);
            else if (!positional && (arg == "-o" || arg == "--output")) { output = value(i); explicit_output = true; }
            else if (!positional && arg != "-" && arg.starts_with('-')) throw std::invalid_argument("unknown option " + arg);
            else if (input.empty()) input = arg;
            else throw std::invalid_argument("expected one input file");
        }
        if (input.empty()) { usage(std::cerr); return 2; }
        if (check && (explicit_target || explicit_output || !options.namespace_name.empty()))
            throw std::invalid_argument("--check cannot be combined with generation options");
        auto doc = [&] {
            if (input != "-") return sandstone::archml::Document::load(input);
            std::string text{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
            if (std::cin.bad()) throw std::runtime_error("cannot read stdin");
            return sandstone::archml::Document::parse(text, "<stdin>");
        }();
        if (check) return 0;
        const auto text = sandstone::archml::generate(doc, target, options);
        if (output == "-") { std::cout << text; std::cout.flush(); if (!std::cout) throw std::runtime_error("cannot write stdout"); }
        else {
            // Generate and validate fully before opening any output file.
            if (input != "-" && std::filesystem::exists(output) && std::filesystem::equivalent(input, output))
                throw std::invalid_argument("output must not overwrite the input profile");
            std::ofstream file(output, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("cannot open output " + output);
            file << text; file.close();
            if (!file) throw std::runtime_error("cannot write output " + output);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "archml: " << error.what() << '\n';
        return 1;
    }
}
