#include "Amalgam8Base.hpp"
#include "Publication.hpp"

#include <iostream>

namespace {
constexpr auto usage =
    "usage: amalgam8 [--api|--cli] [--deps] [--bundle] [--include-root DIR] Foo.am8\n"
    "  --api              emit Foo.hpp (default)\n"
    "  --cli              emit a self-contained Foo.cpp with main\n"
    "  --deps             print the resolved dependency report after publication\n"
    "  --bundle           inline eligible library headers\n"
    "  --include-root DIR look for bundling inputs below DIR (default: current directory)\n"
    "  -h, --help         show this help\n";

const char* context_label(am8::Context context) {
    return context == am8::Context::c ? "C" : context == am8::Context::cpp ? "C++" : "compiler";
}

void report(const am8::Result& result) {
    for (const auto& d : result.dependencies) {
        const auto kind = d.kind == am8::DependencyKind::system ? "system" :
                          d.kind == am8::DependencyKind::bundled ? "bundled" : "external";
        std::cout << kind << '\t' << context_label(d.context) << '\t' << d.name << '\t' << d.header;
        for (const auto& i : d.include_roots) std::cout << "\tinclude=" << i;
        for (const auto& l : d.libraries) std::cout << "\tlibrary=" << l;
        if (!d.conditions.empty()) std::cout << "\tconditions=" << d.conditions;
        std::cout << '\n';
    }
    for (const auto& fragment : result.opaque_fragments)
        std::cout << "opaque\t" << context_label(fragment.context) << '\t' << fragment.role
                  << '\t' << am8::fs::path(fragment.span.file).filename().string()
                  << ':' << fragment.span.line << ':' << fragment.span.column << '\n';
}
}

int main(int argc, char** argv) {
    using namespace am8;
    auto usage_error = [](std::string_view message) {
        std::cerr << "amalgam8: " << message << '\n' << usage;
        return 2;
    };
    Options options;
    bool selected_mode = false, deps = false, positional = false;
    fs::path input;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (!positional && (arg == "-h" || arg == "--help")) {
            std::cout << usage;
            return 0;
        }
        if (!positional && arg == "--") positional = true;
        else if (!positional && (arg == "--cli" || arg == "--api")) {
            if (selected_mode) return usage_error("select exactly one output mode");
            selected_mode = true;
            options.mode = arg == "--cli" ? Mode::cli : Mode::api;
        } else if (!positional && arg == "--bundle") options.bundle = true;
        else if (!positional && arg == "--deps") deps = true;
        else if (!positional && arg == "--include-root") {
            if (++i == argc || std::string_view(argv[i]).starts_with('-'))
                return usage_error("--include-root requires a directory");
            options.include_root = argv[i];
        } else if (!positional && arg.starts_with('-')) {
            return usage_error("unknown option '" + std::string(arg) + "'");
        } else if (input.empty()) input = arg;
        else return usage_error("expected exactly one input file");
    }
    if (input.empty() || input.extension() != ".am8") return usage_error("expected an .am8 input");
    try {
        auto result = compile_file(input, options);
        auto output = input;
        output.replace_extension(options.mode == Mode::cli ? ".cpp" : ".hpp");
        publish(output, result.source);
        if (deps) report(result);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
