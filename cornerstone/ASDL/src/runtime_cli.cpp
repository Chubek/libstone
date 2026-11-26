#include <asdl/execution.hpp>
#include <asdl/lua.hpp>
#include <asdl/schema.hpp>

#include "asdl_loader.h"
#include "asdl_runtime_paths.hpp"
#include "lua_vendor.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr auto usage =
    "Usage: asdl-run [-I DIR] [--stdlib DIR] SCHEMA SCRIPT [ARGS...]\n"
    "       asdl-run [-I DIR] [--stdlib DIR] --check SCHEMA [SCRIPT]\n"
    "       asdl-run --help\n"
    "Use '-' for a driver script read from stdin. Options precede SCHEMA.\n";

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Command {
    bool help = false;
    bool check = false;
    fs::path stdlib;
    std::vector<fs::path> imports;
    fs::path schema;
    fs::path script;
    std::vector<std::string> arguments;
};

Command parse_command(int argc, char** argv) {
    Command result;
    int i = 1;
    const auto option_value = [&](std::string_view opt) -> fs::path {
        if (++i == argc || std::string_view(argv[i]).empty())
            throw UsageError(std::string(opt) + " requires a directory");
        return argv[i];
    };
    for (; i < argc; ++i) {
        const std::string_view word(argv[i]);
        if (word == "--help" || word == "-h") {
            result.help = true;
            return result;
        }
        if (word == "--") {
            ++i;
            break;
        }
        if (word == "--check") result.check = true;
        else if (word == "--stdlib") result.stdlib = option_value(word);
        else if (word == "-I" || word == "--import-root") result.imports.push_back(option_value(word));
        else if (word.compare(0, 2, "-I") == 0) result.imports.emplace_back(word.substr(2));
        else if (!word.empty() && word.front() == '-')
            throw UsageError("unknown option: " + std::string(word));
        else break;
    }
    if (i < argc) result.schema = argv[i++];
    if (i < argc) result.script = argv[i++];
    if (result.schema.empty() || (!result.check && result.script.empty()))
        throw UsageError(result.check ? "missing SCHEMA" : "expected SCHEMA and SCRIPT");
    if (result.check && i != argc) throw UsageError("--check does not take script arguments");
    for (; i < argc; ++i) result.arguments.emplace_back(argv[i]);
    return result;
}

fs::path executable_path(const fs::path& invocation) {
    std::error_code ec;
#if defined(__linux__)
    auto proc_path = fs::canonical("/proc/self/exe", ec);
    if (!ec) return proc_path;
#endif
    if (invocation.has_parent_path()) {
        auto path = fs::canonical(invocation, ec);
        if (!ec) return path;
    } else if (const char* path_var = std::getenv("PATH")) {
#if defined(_WIN32)
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::string_view path(path_var);
        while (true) {
            const auto next = path.find(separator);
            const fs::path directory(path.substr(0, next));
            auto candidate = fs::canonical(directory / invocation, ec);
            if (!ec && fs::is_regular_file(candidate, ec)) return candidate;
            if (next == path.npos) break;
            path.remove_prefix(next + 1);
        }
    }
    throw std::runtime_error("cannot locate asdl-run; pass --stdlib DIR");
}

bool has_stdlib(const fs::path& root) {
    std::error_code ec;
    return fs::is_directory(root / "std", ec);
}

fs::path locate_stdlib(const fs::path& explicit_root, const fs::path& invocation) {
    if (!explicit_root.empty()) {
        if (!has_stdlib(explicit_root))
            throw std::runtime_error("no std/ directory under " + explicit_root.string());
        return fs::canonical(explicit_root);
    }
    const auto bin = executable_path(invocation).parent_path();
    for (const auto* candidate : {asdl::detail::installed_stdlib, asdl::detail::build_stdlib}) {
        const auto root = bin / candidate;
        if (has_stdlib(root)) return fs::canonical(root);
    }
    throw std::runtime_error("cannot locate the ASDL standard library; pass --stdlib DIR");
}

std::string read_script(std::istream& stream, std::string_view path) {
    std::array<char, 8192> buffer;
    std::string result;
    while (stream.read(buffer.data(), buffer.size()))
        result.append(buffer.data(), buffer.size());
    if (!stream.eof() || stream.bad())
        throw std::runtime_error("cannot read script: " + std::string(path));
    result.append(buffer.data(), static_cast<std::size_t>(stream.gcount()));
    return result;
}

int attach_loader(lua_State* state) {
    const auto* stdlib = static_cast<const std::string*>(lua_touserdata(state, 2));
    return asdl_loader_install(state, 1, stdlib->c_str());
}

int attach_execution(lua_State* state) {
    const auto* schema = static_cast<const asdl::Schema*>(lua_touserdata(state, 2));
    try {
        asdl::push_execution(state, *schema);
        lua_getfield(state, -1, "run");
        lua_setfield(state, 1, "run_logistics");
        lua_pop(state, 1);
        return 0;
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}
} // namespace

int main(int argc, char** argv) try {
    auto cmd = parse_command(argc, argv);
    if (cmd.help) {
        std::cout << usage;
        return 0;
    }

    auto schema = asdl::parse_file(cmd.schema, cmd.imports);
    asdl::resolve_options(schema);
    asdl::validate(schema);

    auto stdlib = locate_stdlib(cmd.stdlib, argv[0]).string();
    asdl::LuaOptions lua;
    lua.arguments = std::move(cmd.arguments);
    asdl::LuaSession session(schema, std::move(lua));
    session.extend(attach_loader, &stdlib);
    session.extend(attach_execution, &schema);
    if (cmd.script.empty()) return 0;

    std::string script;
    std::string chunk;
    if (cmd.script == "-") {
        script = read_script(std::cin, "<stdin>");
        chunk = "=<stdin>";
    } else {
        std::ifstream file(cmd.script, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open script: " + cmd.script.string());
        script = read_script(file, cmd.script.string());
        chunk = "@" + cmd.script.string();
    }
    if (cmd.check) session.check(script, chunk);
    else session.run(script, chunk);
    return 0;
} catch (const UsageError& error) {
    std::cerr << "asdl-run: " << error.what() << '\n' << usage;
    return 2;
} catch (const std::exception& error) {
    std::cerr << "asdl-run: " << error.what() << '\n';
    return 1;
}
