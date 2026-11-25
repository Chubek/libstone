#include "CLI.hpp"
#include "WASM-Link.hpp"
#include "WASM-Object-Link.hpp"

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;

int main(int argc, char** argv) {
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (argc < 2 || help) {
        (help ? std::cout : std::cerr)
            << "usage: wasm-link INPUT ... | wasm-link NAME=INPUT ...\n"
               "       wasm-link [--export SYMBOL] ... OBJECT ...\n"
               "  Use named finalized modules for exact import namespace matching.\n"
               "  Positional wasm32 objects are linked using their symbol and relocation sections.\n";
        return help ? 0 : 2;
    }
    try {
        std::vector<core::Module> modules;
        std::vector<std::string_view> names;
        std::vector<std::string> paths;
        std::vector<std::vector<std::uint8_t>> binaries;
        std::vector<std::string_view> exports;
        const auto capacity = static_cast<std::size_t>(argc - 1);
        modules.reserve(capacity);
        names.reserve(capacity);
        paths.reserve(capacity);
        binaries.reserve(capacity);
        bool used_stdin = false;
        bool options = true;
        std::optional<bool> named;
        std::optional<bool> objects;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (options && arg == "--") {
                options = false;
                continue;
            }
            if (options && (arg == "--export" || arg.starts_with("--export="))) {
                const auto name = arg == "--export"
                    ? (++i < argc ? std::string_view(argv[i]) : std::string_view{})
                    : arg.substr(9);
                if (name.empty())
                    throw std::runtime_error("--export requires a symbol name");
                exports.push_back(name);
                continue;
            }
            const auto equal = arg.find('=');
            const bool is_named = equal != arg.npos;
            if (named && *named != is_named)
                throw std::runtime_error("cannot mix named and positional inputs");
            named = is_named;
            if (is_named && (equal == 0 || equal + 1 == arg.size()))
                throw std::runtime_error("expected NAME=INPUT");
            const std::string current_path(is_named ? arg.substr(equal + 1) : arg);
            if (current_path == "-") {
                if (used_stdin)
                    throw std::runtime_error("stdin can only be read once per link");
                used_stdin = true;
            }
            const auto data = wasm::detail::read_input(current_path.c_str());
            std::vector<std::uint8_t> bytes(data.begin(), data.end());
            core::Module module;
            try {
                module = core::decode_module(bytes);
            } catch (const wasm::Error& e) {
                std::cerr << "wasm-link: " << current_path << ": " << e.what() << '\n';
                return 1;
            }
            bool is_object = false;
            for (const auto& section : module.custom)
                is_object |= section.name == "linking" || section.name.starts_with("reloc.");
            if (is_named && is_object)
                throw std::runtime_error(current_path + ": named inputs are not supported for Wasm objects");
            if (objects && *objects != is_object)
                throw std::runtime_error(current_path + ": cannot mix relocatable Wasm objects and finalized modules");
            objects = is_object;
            if (is_object) {
                paths.push_back(current_path);
                binaries.push_back(std::move(bytes));
            } else {
                modules.push_back(std::move(module));
                if (is_named)
                    names.push_back(arg.substr(0, equal));
            }
        }
        if (!objects)
            throw std::runtime_error("no input files");
        if (*objects) {
            std::vector<core::ObjectLinkInput> inputs;
            inputs.reserve(paths.size());
            for (std::size_t i = 0; i < paths.size(); ++i)
                inputs.push_back({paths[i], binaries[i]});
            wasm::detail::write_binary(core::encode_module(core::link_objects(inputs, {exports})));
        } else if (!exports.empty()) {
            throw std::runtime_error("--export is only supported when linking Wasm objects");
        } else if (*named) {
            std::vector<core::LinkInput> inputs;
            inputs.reserve(names.size());
            for (std::size_t i = 0; i < names.size(); ++i)
                inputs.push_back({names[i], &modules[i]});
            wasm::detail::write_binary(core::encode_module(core::link_modules(inputs)));
        } else {
            wasm::detail::write_binary(core::encode_module(core::link_modules(modules)));
        }
    } catch (const std::exception& error) {
        std::cerr << "wasm-link: " << error.what() << '\n';
        return 1;
    }
}
