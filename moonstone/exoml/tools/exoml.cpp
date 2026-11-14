#include <exoml/StaticFFI.hpp>
#include <fstream>
#include <iostream>

using namespace moonstone::exoml;
namespace {
bool run(Interpreter &vm, const std::string &text, const std::string &file, bool check) {
    try {
        auto module = Parser(text, file).parse();
        if (check)
            return true;
        auto errors = vm.execute(module, {.stop_on_error = true});
        for (const auto &error : errors)
            std::cerr << moonstone::format(error) << '\n';
        return errors.empty();
    } catch (const ParseError &error) {
        std::cerr << file << ':' << error.source.line << ':' << error.source.column << ": "
                  << error.what() << '\n';
        return false;
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool check = false, repl = false;
        std::string emit, file;
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--help") {
                std::cout
                    << "Usage: exoml [--check] [--emit SYMBOL] FILE|-\n       exoml --repl\n"
                       "Evaluate declarative StaticFFI scripts. --check parses only.\n"
                       "--emit writes one result verbatim; --repl accepts one module per line.\n";
                return 0;
            } else if (option == "--check")
                check = true;
            else if (option == "--repl")
                repl = true;
            else if (option == "--emit" && i + 1 < argc)
                emit = argv[++i];
            else if ((option == "-" || !option.starts_with('-')) && file.empty())
                file = option;
            else
                throw std::invalid_argument("unknown or incomplete option: " + option);
        }
        Interpreter vm;
        install_static_ffi(vm);
        if (repl) {
            if (!file.empty() || check || !emit.empty())
                throw std::invalid_argument("--repl does not accept file/check/emit options");
            std::string line;
            while (std::cout << "exo> " && std::getline(std::cin, line)) {
                if (line == ":quit")
                    break;
                if (line != ":symbols" && !run(vm, line, "<repl>", false))
                    continue;
                for (const auto &[name, value] : vm.symbols())
                    std::cout << name << " = " << stringify(value) << '\n';
            }
            return 0;
        }
        if (file.empty())
            throw std::invalid_argument("expected input file (use --help)");
        std::ifstream input;
        if (file != "-") {
            input.open(file, std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot open " + file);
        }
        std::istream &stream = file == "-" ? std::cin : input;
        std::string text;
        char c;
        while (stream.get(c)) {
            if (text.size() == ParseLimits{}.input_bytes)
                throw std::length_error("input byte budget exceeded");
            text += c;
        }
        if (!stream.eof())
            throw std::runtime_error("input read failed");
        if (!run(vm, text, file, check))
            return 1;
        if (!check) {
            if (!emit.empty()) {
                auto found = vm.symbols().find(emit);
                if (found == vm.symbols().end())
                    throw std::invalid_argument("unknown output symbol: " + emit);
                std::cout << stringify(found->second);
            } else
                for (const auto &[name, value] : vm.symbols())
                    std::cout << name << " = " << stringify(value) << '\n';
        }
        if (!std::cout)
            throw std::runtime_error("output write failed");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "exoml: " << error.what() << '\n';
        return 1;
    }
}
