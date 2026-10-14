// sandstone-spec-compiler — Symspec documents -> SymAS banks / backend JSON.
//
// Pipeline:
//
//   .dsymspec (Lua DSL, run by scripts/spec-compiler.py under stock Lua)
//       -> JSON document(s) -> domsymas::Builder -> .symas
//                              (spec_document.cpp, no interpreter needed)
//
// Multiple JSON inputs are folded into a single databank, so one bank may
// describe several instruction sets, syntaxes, and object formats. This
// JSON and generated backend modules use Boost.JSON through Symspec.hpp.
// Original DSL sources use stock Lua or the optional DomMune driver.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "domsymascc/domsymascc.hpp"
#include "sandstone.hpp"

namespace {

void usage(const char *argv0) {
    std::cerr
        << "usage: " << argv0
        << " [-o OUT.symas] [--compression none|zlib|zstd] [--validate]\n"
        << "                  [--emit-spec [FILE] | --emit-backend [FILE]]\n"
        << "                  [--spec-dir DIR] [--dsl-dir DIR] [--lua LUA] inputs...\n"
        << "\n"
        << "Load JSON, .dsymspec Lua sources, or generated backend .lua modules.\n"
        << "Assembler documents compile into SymAS banks; backend-only modules\n"
        << "can be validated and exported with --emit-backend.\n"
        << "\n"
        << "  -o OUT            output .symas path (required with several inputs\n"
        << "                    or --emit-spec to a file; with one input the\n"
        << "                    default replaces the input suffix with .symas)\n"
        << "  --compression C   none, zlib (default), or zstd\n"
        << "  --validate        reload the bank and run semantic validation\n"
        << "  --emit-spec [F]   write one JSON document per input to F ('-' or\n"
        << "                    absent = stdout) instead of building a bank\n"
        << "  --emit-backend [F] export backend JSON (one document per input)\n"
        << "  --spec-dir DIR    extra Lua module directory for backend modules\n"
        << "  --dsl-dir DIR     directory containing the external dsl.lua runtime\n"
        << "  --lua LUA         Lua executable (default: lua)\n"
        << "  --help            print this text\n";
}

std::string default_output(const std::string &input) {
    auto path = std::filesystem::path(input);
    path.replace_extension(".symas");
    return path.string();
}

} // namespace

int main(int argc, char **argv) {
    std::string output;
    std::string compression = "zlib";
    std::string spec_dir;
    bool validate = false;
    bool emit_spec = false;
    bool emit_backend = false;
    symspec::LoadOptions load_options;
    if (const char *directory = std::getenv("SANDSTONE_DSL_DIRECTORY"))
        load_options.lua.dsl_directory = directory;
    std::string emit_dest = "-";
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need_value = [&](const char *flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "sandstone-spec-compiler: " << flag
                          << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else if (a == "-o" || a == "--output") {
            output = need_value("-o");
        } else if (a == "--compression") {
            compression = need_value("--compression");
        } else if (a == "--validate") {
            validate = true;
        } else if (a == "--emit-spec" || a == "--emit-backend") {
            if (emit_spec || emit_backend) {
                std::cerr << "sandstone-spec-compiler: choose one export mode\n";
                return 2;
            }
            emit_spec = a == "--emit-spec";
            emit_backend = a == "--emit-backend";
            if (i + 1 < argc && argv[i + 1][0] != '-')
                emit_dest = argv[++i];
            else if (i + 1 < argc && std::string(argv[i + 1]) == "-")
                emit_dest = argv[++i];
        } else if (a == "--spec-dir") {
            spec_dir = need_value("--spec-dir");
        } else if (a == "--dsl-dir") {
            load_options.lua.dsl_directory = need_value("--dsl-dir");
        } else if (a == "--lua") {
            load_options.lua.executable = need_value("--lua");
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "sandstone-spec-compiler: unknown option '" << a
                      << "'\n";
            return 2;
        } else {
            inputs.push_back(a);
        }
    }
    if (!spec_dir.empty()) load_options.lua.module_directories.emplace_back(spec_dir);

    if (inputs.empty()) {
        std::cerr << "sandstone-spec-compiler: at least one input is "
                     "required\n";
        return 2;
    }

    std::vector<symspec::Spec> documents;
    for (const auto &input : inputs) {
        auto spec = symspec::Spec::load(input, load_options);
        if (!spec) {
            std::cerr << "sandstone-spec-compiler: " << input << ": "
                      << spec.error().message << "\n";
            return 1;
        }
        documents.push_back(std::move(*spec));
    }

    if (emit_spec || emit_backend) {
        std::string joined;
        for (size_t i = 0; i < documents.size(); i++) {
            if (i)
                joined += "\n";
            auto json = documents[i].serialize(emit_backend ? symspec::OutputFormat::backend_json
                                                           : symspec::OutputFormat::json);
            if (!json) {
                std::cerr << "sandstone-spec-compiler: " << inputs[i] << ": "
                          << json.error().message << "\n";
                return 1;
            }
            joined += *json;
        }
        if (emit_dest == "-") {
            std::cout << joined;
            if (!joined.empty() && joined.back() != '\n')
                std::cout << "\n";
        } else {
            std::ofstream f(emit_dest, std::ios::trunc | std::ios::binary);
            if (!f) {
                std::cerr << "sandstone-spec-compiler: cannot write '"
                          << emit_dest << "'\n";
                return 1;
            }
            f << joined << "\n";
            f.close();
            if (!f) return 1;
        }
        return std::cout ? 0 : 1;
    }

    domsymas::Builder builder(true);
    if (compression == "none")
        builder.set_compression(domsymas::Compression::None);
    else if (compression == "zlib")
        builder.set_compression(domsymas::Compression::Zlib);
    else if (compression == "zstd")
        builder.set_compression(domsymas::Compression::Zstd);
    else {
        std::cerr << "sandstone-spec-compiler: unknown compression '"
                  << compression << "' (none, zlib, or zstd)\n";
        return 2;
    }

    for (size_t i = 0; i < documents.size(); i++) {
        domsymas::cc::Result r =
            domsymas::cc::compile_spec(documents[i], builder);
        if (!r.ok) {
            std::cerr << "sandstone-spec-compiler: " << inputs[i] << ": "
                      << r.error << "\n";
            return 1;
        }
    }

    std::string out_path = output.empty() && inputs.size() == 1
                               ? default_output(inputs[0])
                               : output;
    if (out_path.empty()) {
        std::cerr << "sandstone-spec-compiler: multiple inputs require "
                     "-o/--output\n";
        return 2;
    }

    domsymas::Error e = builder.save_file(out_path);
    if (!e.ok()) {
        std::cerr << "sandstone-spec-compiler: " << e.message << "\n";
        return 1;
    }

    if (validate) {
        domsymas::Databank bank;
        domsymas::Error le = bank.load_file(out_path);
        if (!le.ok()) {
            std::cerr << "sandstone-spec-compiler: validate failed: "
                      << le.message << "\n";
            return 1;
        }
        std::string dg;
        domsymas::Error ve = bank.validate(true, &dg);
        if (!ve.ok()) {
            std::cerr << "sandstone-spec-compiler: validate failed: " << dg
                      << "\n";
            return 1;
        }
        std::cout << "validated: " << out_path << "\n";
    } else {
        std::cout << "wrote: " << out_path << "\n";
    }
    return 0;
}
