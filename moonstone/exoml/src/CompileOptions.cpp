#include "CompileOptions.hpp"
#include "ExtractCache.hpp"
#include <filesystem>
#include <fstream>
#include <json.hpp>

namespace moonstone::exoml::cdecl::detail {
namespace {
using Json = nlohmann::json;
void clean(std::string_view s) {
    if (s.find_first_of(std::string_view("\0\r\n", 3)) != std::string_view::npos)
        throw std::invalid_argument("NUL/newline in compilation option");
}
bool identifier(std::string_view s) {
    auto alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return !s.empty() && alpha(s.front()) && std::all_of(s.begin(), s.end(), [&](char c) {
        return alpha(c) || (c >= '0' && c <= '9');
    });
}
std::string absolute(const std::string &value, const std::filesystem::path &directory) {
    clean(value);
    if (value.empty())
        throw std::invalid_argument("empty compiler path");
    auto p = std::filesystem::path(value);
    return std::filesystem::weakly_canonical(p.is_absolute() ? p : directory / p).string();
}
std::vector<std::string> words(std::string_view command, std::size_t limit) {
    clean(command);
    std::vector<std::string> out;
    std::string word;
    char quote = 0;
    bool started = false;
    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote == '\'') {
            if (c == '\'')
                quote = 0;
            else
                word += c;
        } else if (c == '\\') {
            if (++i == command.size())
                throw std::invalid_argument("unterminated command escape");
            char next = command[i];
            if (quote == '"' && next != '"' && next != '\\' && next != '$' && next != '`')
                word += '\\';
            word += next;
            started = true;
        } else if (c == '$' || c == '`') {
            throw std::invalid_argument(
                "shell expansion is unsupported in compile commands; use arguments[]");
        } else if (quote == '"') {
            if (c == '"')
                quote = 0;
            else
                word += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            started = true;
        } else if (c == ' ' || c == '\t') {
            if (started) {
                if (out.size() >= limit)
                    throw std::length_error("compile command argument budget exceeded");
                out.push_back(std::move(word));
                word.clear();
                started = false;
            }
        } else {
            if (std::string_view(";&|<>").find(c) != std::string_view::npos)
                throw std::invalid_argument("shell operators are unsupported in compile commands");
            word += c;
            started = true;
        }
    }
    if (quote)
        throw std::invalid_argument("unterminated command quote");
    if (started) {
        if (out.size() >= limit)
            throw std::length_error("compile command argument budget exceeded");
        out.push_back(std::move(word));
    }
    return out;
}
struct Selected {
    CompilationContext context;
    InputFile input;
};
Selected select(const ExtractOptions &o) {
    auto database = std::filesystem::canonical(o.compile_commands);
    auto size = std::filesystem::file_size(database);
    if (size > o.limits.input_bytes)
        throw std::length_error("compilation database byte budget exceeded");
    std::ifstream stream(database, std::ios::binary);
    std::string bytes(std::size_t(size), '\0');
    if (!stream.read(bytes.data(), std::streamsize(size)) ||
        stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("cannot read stable compilation database");
    std::vector<std::set<std::string>> keys;
    std::size_t nodes = 0;
    auto json = Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json &value) {
        if (depth > 64 || nodes++ >= o.limits.entries)
            throw std::length_error("compilation database structure budget exceeded");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == Json::parse_event_t::key &&
            !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("duplicate compilation database key");
        if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
    if (!json.is_array() || json.empty())
        throw std::invalid_argument("expected nonempty compilation database array");
    std::optional<CompilationContext> chosen;
    auto selector = o.compile_file.empty()
                        ? std::string{}
                        : absolute(o.compile_file, std::filesystem::current_path());
    auto header = absolute(o.header, std::filesystem::current_path());
    for (const auto &entry : json) {
        if (!entry.is_object())
            throw std::invalid_argument("compilation entry must be an object");
        for (auto it = entry.begin(); it != entry.end(); ++it)
            if (it.key() != "file" && it.key() != "directory" && it.key() != "arguments" &&
                it.key() != "command" && it.key() != "output")
                throw std::invalid_argument("unsupported compilation entry field: " + it.key());
        if (!entry.contains("directory") || !entry["directory"].is_string() ||
            !entry.contains("file") || !entry["file"].is_string() ||
            entry.contains("arguments") == entry.contains("command") ||
            (entry.contains("output") && !entry["output"].is_string()))
            throw std::invalid_argument(
                "compilation entry requires directory, file and exactly one arguments/command");
        CompilationContext context;
        context.database = database.string();
        context.directory = absolute(entry["directory"].get<std::string>(), database.parent_path());
        if (!std::filesystem::is_directory(context.directory))
            throw std::invalid_argument("compilation working directory is not a directory");
        context.file = absolute(entry["file"].get<std::string>(), context.directory);
        bool matches = selector.empty() ? (json.size() == 1 || context.file == header)
                                        : context.file == selector;
        if (!matches)
            continue;
        if (chosen)
            throw std::invalid_argument(
                "ambiguous compilation entries; select a unique source/configuration");
        if (entry.contains("arguments")) {
            if (!entry["arguments"].is_array())
                throw std::invalid_argument("arguments must be a string array");
            for (const auto &a : entry["arguments"]) {
                if (!a.is_string())
                    throw std::invalid_argument("compile argument must be a string");
                auto value = a.get<std::string>();
                clean(value);
                context.original_arguments.push_back(std::move(value));
            }
        } else {
            if (!entry["command"].is_string())
                throw std::invalid_argument("command must be a string");
            context.original_arguments =
                words(entry["command"].get<std::string>(), o.limits.entries);
        }
        if (context.original_arguments.empty() || context.original_arguments.front().empty())
            throw std::invalid_argument("missing compiler driver");
        context.driver = context.original_arguments.front(); // Recorded, never executed.
        auto extension = std::filesystem::path(context.file).extension().string();
        bool explicit_c = false;
        for (std::size_t i = 1; i + 1 < context.original_arguments.size(); ++i)
            if (context.original_arguments[i] == "-x" &&
                (context.original_arguments[i + 1] == "c" ||
                 context.original_arguments[i + 1] == "c-header"))
                explicit_c = true;
        if (extension != ".c" && extension != ".h" && !explicit_c)
            throw std::invalid_argument(
                "non-C compilation source requires an explicit -x c context");
        chosen = std::move(context);
    }
    if (!chosen)
        throw std::invalid_argument("no unique compilation entry; specify --compile-file");
    return {std::move(*chosen), {database.string(), sha256(bytes)}};
}
void normalize(std::vector<std::string> args, const std::filesystem::path &directory,
               std::vector<std::string> &out, CompilationContext *context = nullptr) {
    std::size_t sources = 0;
    auto ignore = [&](const std::string &a) {
        if (context)
            context->ignored_arguments.push_back(a);
    };
    for (std::size_t i = context ? 1 : 0; i < args.size(); ++i) {
        const auto &a = args[i];
        clean(a);
        auto next = [&]() -> const std::string & {
            if (++i == args.size())
                throw std::invalid_argument("missing compiler argument for " + a);
            clean(args[i]);
            return args[i];
        };
        if (context && !a.starts_with('-') && !a.starts_with('@') &&
            absolute(a, directory) == context->file) {
            ++sources;
            ignore(a);
            continue;
        }
        if (a == "-c" || a == "-S" || a == "-fsyntax-only" || a == "-MD" || a == "-MMD" ||
            a == "-MP") {
            if (!context)
                throw std::invalid_argument(
                    "driver actions are accepted only in compilation database entries");
            ignore(a);
            continue;
        }
        if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") {
            if (!context)
                throw std::invalid_argument(
                    "output actions are accepted only in compilation database entries");
            ignore(a);
            ignore(next());
            continue;
        }
        if (a.starts_with("-MF") || a.starts_with("-MT") || a.starts_with("-MQ")) {
            if (!context)
                throw std::invalid_argument(
                    "dependency actions are accepted only in compilation database entries");
            ignore(a);
            continue;
        }
        if (a == "-I" || a == "-isystem" || a == "-iquote" || a == "-idirafter" ||
            a == "-include" || a == "-imacros" || a == "-isysroot" || a == "--sysroot" ||
            a == "-resource-dir") {
            auto value = absolute(next(), directory);
            out.push_back(a);
            out.push_back(std::move(value));
            continue;
        }
        if (a.starts_with("-I") && a.size() > 2) {
            out.push_back("-I" + absolute(a.substr(2), directory));
            continue;
        }
        if (a.starts_with("--sysroot=") || a.starts_with("-resource-dir=")) {
            auto split = a.find('=');
            out.push_back(a.substr(0, split + 1) + absolute(a.substr(split + 1), directory));
            continue;
        }
        if (a == "-D" || a == "-U" || a.starts_with("-D") || a.starts_with("-U")) {
            char kind = a[1];
            auto value = a.size() == 2 ? next() : a.substr(2);
            auto name = value.substr(0, value.find('='));
            if (!identifier(name) || (kind == 'U' && name != value) ||
                name.starts_with("MULIB_IMPL_"))
                throw std::invalid_argument(
                    "invalid macro option or forbidden MULIB_IMPL_* switch: " + value);
            out.push_back(std::string("-") + kind + value);
            continue;
        }
        if (a == "-target" || a == "--target") {
            auto target = next();
            if (target.empty())
                throw std::invalid_argument("empty compiler target");
            out.push_back("--target=" + target);
            continue;
        }
        if (a.starts_with("--target=")) {
            if (a.size() == 9)
                throw std::invalid_argument("empty compiler target");
            out.push_back(a);
            continue;
        }
        if (a == "-x") {
            auto language = next();
            if (language != "c" && language != "c-header")
                throw std::invalid_argument("discovery requires a C11 compilation entry");
            out.push_back("-x");
            out.push_back(language);
            continue;
        }
        static const std::set<std::string> flags = {"-std=c11",
                                                    "-std=gnu11",
                                                    "-std=iso9899:2011",
                                                    "-m32",
                                                    "-m64",
                                                    "-mx32",
                                                    "-fshort-enums",
                                                    "-fno-short-enums",
                                                    "-fshort-wchar",
                                                    "-fno-short-wchar",
                                                    "-fsigned-char",
                                                    "-funsigned-char",
                                                    "-fpack-struct",
                                                    "-fpack-struct=1",
                                                    "-fpack-struct=2",
                                                    "-fpack-struct=4",
                                                    "-fpack-struct=8",
                                                    "-fpack-struct=16",
                                                    "-malign-double",
                                                    "-mms-bitfields",
                                                    "-mno-ms-bitfields",
                                                    "-fms-extensions",
                                                    "-fno-ms-extensions",
                                                    "-mfloat-abi=soft",
                                                    "-mfloat-abi=softfp",
                                                    "-mfloat-abi=hard",
                                                    "-pthread",
                                                    "-fno-builtin",
                                                    "-fPIC",
                                                    "-fpic",
                                                    "-fPIE",
                                                    "-fpie",
                                                    "-O0",
                                                    "-O1",
                                                    "-O2",
                                                    "-O3",
                                                    "-Os",
                                                    "-Oz",
                                                    "-Og",
                                                    "-g",
                                                    "-g0",
                                                    "-g1",
                                                    "-g2",
                                                    "-g3",
                                                    "-pedantic",
                                                    "-pedantic-errors",
                                                    "-nostdinc"};
        if (flags.contains(a)) {
            out.push_back(a);
            continue;
        }
        bool target_flag = a.starts_with("-mabi=") || a.starts_with("-march=") ||
                           a.starts_with("-mcpu=") || a.starts_with("-mtune=");
        auto split = a.find('=');
        if (target_flag && split + 1 < a.size() &&
            std::all_of(a.begin() + std::ptrdiff_t(split + 1), a.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '+';
            })) {
            out.push_back(a);
            continue;
        }
        if (a.starts_with("-W") && !a.starts_with("-Wl") && a.size() > 2 &&
            std::all_of(a.begin() + 2, a.end(),
                        [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '='; })) {
            out.push_back(a);
            continue;
        }
        throw std::invalid_argument("unsupported/unsafe compiler argument: " + a);
    }
    if (context && sources != 1)
        throw std::invalid_argument("compile command must name exactly its selected source once");
}
} // namespace
CompileOptions compilation_options(const ExtractOptions &o) {
    CompileOptions result;
    auto cwd = std::filesystem::current_path();
    if (!o.compile_commands.empty()) {
        clean(o.compile_commands);
        clean(o.compile_file);
        auto selected = select(o);
        result.context = std::move(selected.context);
        result.database = std::move(selected.input);
        normalize(result.context->original_arguments, result.context->directory, result.arguments,
                  &*result.context);
    } else if (!o.compile_file.empty())
        throw std::invalid_argument("--compile-file requires --compile-commands");
    if (o.compiler_arguments.size() > o.limits.entries)
        throw std::length_error("compiler argument count budget exceeded");
    normalize(o.compiler_arguments, cwd, result.arguments);
    for (const auto &include : o.include_directories)
        result.arguments.push_back("-I" + absolute(include, cwd));
    std::vector<std::string> macros;
    for (const auto &d : o.defines)
        macros.push_back("-D" + d);
    for (const auto &u : o.undefines)
        macros.push_back("-U" + u);
    normalize(std::move(macros), cwd, result.arguments);
    clean(o.target);
    clean(o.resource_directory);
    if (!o.target.empty())
        result.arguments.push_back("--target=" + o.target);
    if (!o.resource_directory.empty())
        result.arguments.push_back("-resource-dir=" + absolute(o.resource_directory, cwd));
    if (result.arguments.size() > o.limits.entries)
        throw std::length_error("compiler argument count budget exceeded");
    std::size_t bytes = 0;
    for (const auto &a : result.arguments) {
        if (a.size() > o.limits.input_bytes - std::min(bytes, o.limits.input_bytes))
            throw std::length_error("compiler argument byte budget exceeded");
        bytes += a.size();
    }
    return result;
}
} // namespace moonstone::exoml::cdecl::detail
