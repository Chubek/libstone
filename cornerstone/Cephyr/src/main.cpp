#include "sandslab_opt.hpp"
#include "blackstone_opt.hpp"
#include "driver.hpp"
#include "driver_io.hpp"
#include "embedding_internal.hpp"
#include "mir_verify.h"
#include "moonstone_tools.hpp"
#include "qml_rewrite.hpp"
#include "sdk_internal.hpp"
#include "syntax.hpp"
#include "../FactIR.hpp"
#include "../extension/lua_extensions.hpp"
#include "../extension/native_plugins.hpp"

#ifdef CEPHYR_WITH_MOONSTONE_ANALYSIS
#include "moonstone_analysis.hpp"
#endif

#include <cephyr/Cephyr.h>
#include <cephyr/backend.hpp>
#include <cephyr/config.hpp>
#include <cephyr/diagnostics.hpp>
#include <cephyr/host_target.hpp>
#include <cephyr/lower.hpp>
#include <cephyr/parser.hpp>
#include <cephyr/preprocess.hpp>
#include <cephyr/process.hpp>
#include <cephyr/target.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <locale>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace cephyr {
namespace {

namespace fs = std::filesystem;
using Command = std::vector<std::string>;
using detail::MemoryFile;
using detail::save;
using detail::TempDirectory;

enum class Stage { link, object, assembly, preprocess, syntax, ast, mir };
enum class Language { automatic, c, preprocessed, ast_json, ast_yaml, ast_sexp,
                      assembly, assembly_cpp, link, link_option };

struct Input {
    Language language;
    std::string argument;
    const detail::TranslationUnit *unit = nullptr;

    const std::string &name() const { return unit ? unit->name : argument; }
};

struct Options {
    Stage stage = Stage::link;
    cephyr_ast_format ast_format{};
    std::vector<Input> inputs;
    fs::path output, config_file, sysroot;
    fs::path profile_output;
    fs::path factir_smtlib, factir_facts;
    std::string target, stdlib;
    std::string qml_profile;
    QmlProfile qml;
    std::vector<std::string> lua_files;
    std::vector<std::string> lua_environment;
    Command assembler_options;
    PreprocessOptions preprocess;
    std::optional<bool> use_cache, static_link, line_markers, keep_comments, trigraphs, pic, hosted;
    std::optional<bool> factir_enabled;
    std::optional<bool> sandslab_enabled;
    bool no_headers = false, no_start = false, no_libraries = false, shared = false;
    bool verbose = false, sarif = false, werror = false, help = false, version = false;
    bool allow_native_plugins = false;
    bool moonstone_analysis = false;
};

struct Invocation {
    const cephyr_request &request;
    cephyr_result &result;
    std::istream *cli_input = nullptr;
    std::ostream *cli_output = nullptr;
    std::span<const std::shared_ptr<const detail::TranslationUnit>> units{};
    const detail::HostSymbols *symbols = nullptr;
    cephyr_ast_format ast_format{};

    bool embedding() const { return !cli_output; }
};

constexpr std::string_view help_text = R"(Usage: cephyr [options] file...
Compile C11 for an Archon target (default: x86_64-linux-gnu).

  -E                        Preprocess only
  -fsyntax-only             Check the C frontend without invoking target tools
  --emit-ast=FORMAT         Parse one C11 input and write JSON, YAML or S-expressions
                            (json, yaml, sexp; default output is stdout)
  --emit-mir                Write Archon MachineIR text
  -S                        Compile to assembly
  -c                        Compile/assemble to a relocatable object
  -o FILE                   Write the result to FILE (default link: a.out)
  -x LANG                   c, cpp-output, assembler, assembler-with-cpp, none
                            c11-ast-json, c11-ast-yaml, c11-ast-sexp
  AST file suffixes: .json, .yaml, .yml, .sexp, .sexpr, .sxp
  -std=c11                  Select ISO C11
  -I DIR, -isystem DIR      Add user or system include paths
  -D NAME[=VALUE], -U NAME  Define or undefine a macro
  -include FILE             Include a header before the input
  -P, -C                    Omit line markers or preserve comments in -E
  -ffreestanding            Select freestanding C translation
  -nostdinc, -nostdlib      Omit default headers or startup and libraries
  -static, -dynamic         Select static or dynamic linkage
  -shared, -fPIC            Link a shared object or emit position-independent code
  -L DIR, -l NAME           Add a linker library path or library
  -Wa,ARG,... -Wl,ARG,...   Pass arguments to the assembler or linker
  --sysroot DIR             Set the target sysroot
  --stdlib MODE             cephyr, system, or none
  --target TRIPLE|auto      Select a Lua target or a supported native host target
  --config FILE             Read FILE instead of ./Cephyr.toml
  --no-factir, --factir      Disable or enable verified FactIR (default: on)
  --sandslab, --no-sandslab  Toggle late native AMD64 constant shortening (default: off)
  --factir-smtlib FILE      Write the SMT-LIB obligation (one C input)
  --factir-facts FILE       Write the committed Datalog facts (one C input)
  --profile NAME            Select a QML profile from the configuration
  --qml-pass NAME           Append a QML pass (repeatable)
  --qml-file PATH           Load a QML rule fragment (repeatable)
  --qml-fuel N              Set the QML rewrite fuel limit
  --qml-param PHASE.KEY=N   Set a phase parameter (repeatable; KEY may be fuel)
  --lua-extension FILE      Load a sandboxed Lua extension (repeatable)
  --lua-env NAME=VALUE       Grant Lua extensions an explicit value (repeatable)
  --allow-native-plugins    Load trusted native plugins listed in Cephyr.toml
  --no-cache                Compatibility option (packaged backends need no cache)
  --diagnostics-format=FMT  text or sarif
  --moonstone-analyze       Inspect supported typed MIR (optional build)
  --moonstone-profile=FILE  Write a compiler phase profile (optional build)
  -Werror, -v               Promote warnings, or report compiler stages
  --help, --version         Show this help or the Cephyr version

Moonstone tools (optional build; select as the first argument):
  --moonstone-exoml FILE SYMBOL
  --moonstone-exotract HEADER IMPLEMENTATION_MACRO...
  --moonstone-qmr-compile FILE
  --moonstone-qmr-check [--jobs=N] FILE...
)";

cephyr_ast_format input_ast_format(Language kind)
{
    switch (kind) {
    case Language::ast_json: return CEPHYR_AST_JSON;
    case Language::ast_yaml: return CEPHYR_AST_YAML;
    case Language::ast_sexp: return CEPHYR_AST_SEXP;
    default: return {};
    }
}

bool c_input(Language kind)
{
    return kind == Language::c || kind == Language::preprocessed || input_ast_format(kind);
}

bool source_input(Language kind)
{
    return c_input(kind) || kind == Language::assembly || kind == Language::assembly_cpp;
}

bool object_input(const fs::path &path)
{
    const auto name = path.filename().string();
    return name.ends_with(".o") || name.ends_with(".a") || name.ends_with(".so") ||
        name.find(".so.") != std::string::npos;
}

bool for_stage(Language kind, Stage stage)
{
    if (stage == Stage::link)
        return true;
    if (stage == Stage::syntax || stage == Stage::mir || stage == Stage::ast)
        return c_input(kind);
    return source_input(kind);
}

Language infer_language(const std::string &name)
{
    if (name == "-")
        return Language::c;
    const auto ext = fs::path(name).extension();
    if (ext == ".c") return Language::c;
    if (ext == ".i") return Language::preprocessed;
    if (ext == ".json") return Language::ast_json;
    if (ext == ".yaml" || ext == ".yml") return Language::ast_yaml;
    if (ext == ".sexp" || ext == ".sexpr" || ext == ".sxp") return Language::ast_sexp;
    if (ext == ".s") return Language::assembly;
    if (ext == ".S" || ext == ".sx") return Language::assembly_cpp;
    return Language::link;
}

void extend(Command &to, const Command &from)
{
    to.insert(to.end(), from.begin(), from.end());
}

std::optional<Language> language(std::string_view value)
{
    if (value == "none") return Language::automatic;
    if (value == "c") return Language::c;
    if (value == "cpp-output") return Language::preprocessed;
    if (value == "c11-ast-json") return Language::ast_json;
    if (value == "c11-ast-yaml") return Language::ast_yaml;
    if (value == "c11-ast-sexp" || value == "c11-ast-sexpr") return Language::ast_sexp;
    if (value == "assembler") return Language::assembly;
    if (value == "assembler-with-cpp") return Language::assembly_cpp;
    return std::nullopt;
}

std::optional<Command> comma_arguments(std::string_view text)
{
    Command args;
    while (!text.empty()) {
        const auto comma = text.find(',');
        if (!comma)
            return std::nullopt;
        args.emplace_back(text.substr(0, comma));
        if (comma == std::string_view::npos)
            return args;
        text.remove_prefix(comma + 1);
    }
    return std::nullopt;
}

std::optional<unsigned> positive_unsigned(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    unsigned value;
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end && value ? std::optional{value} : std::nullopt;
}

bool parse_options(std::size_t argc, const char *const *argv, bool embedding,
                   Options &opts, Diagnostics &diag,
                   std::span<const std::shared_ptr<const detail::TranslationUnit>> units,
                   cephyr_ast_format ast_format)
{
    if (ast_format) {
        opts.stage = Stage::ast;
        opts.ast_format = ast_format;
    }
    bool positional = false, input_dash = false;
    Language current = Language::automatic;
    const auto stage = [&](Stage selected) {
        if (opts.stage != Stage::link && opts.stage != selected)
            diag.error("cephyr.option", "conflicting compilation stages");
        opts.stage = selected;
    };
    for (std::size_t i = 0; i < argc; ++i) {
        const std::string_view token(argv[i]);
        const auto argument = [&](std::string_view option) -> std::string {
            if (token.size() > option.size()) {
                auto value = token.substr(option.size());
                if (option.starts_with("--") || option == "-std" ||
                    option == "-fdiagnostics-format")
                    value.remove_prefix(1); // caller matched the exact option or an '=' spelling
                if (!value.empty())
                    return std::string(value);
            } else if (i + 1 < argc && *argv[i + 1] != '\0') {
                return argv[++i];
            }
            diag.error("cephyr.option", "missing argument to '" + std::string(option) + "'");
            return {};
        };
        const auto option = [&](std::string_view name) {
            return token == name || (token.starts_with(name) && token.size() > name.size() &&
                                     token[name.size()] == '=');
        };
        if (positional || !token.starts_with('-') || token == "-") {
            if (token.empty()) {
                diag.error("cephyr.option", "empty input path");
                continue;
            }
            auto type = current == Language::automatic ? infer_language(std::string(token)) : current;
            if (token == "-") {
                if (input_dash)
                    diag.error("cephyr.option", "standard input may only be read once");
                input_dash = true;
                if (!units.empty()) {
                    if (type != Language::c)
                        diag.error("cephyr.option", "the translation-unit operand '-' must select C");
                    for (const auto &unit : units)
                        opts.inputs.push_back({Language::c, "-", unit.get()});
                    continue;
                }
            }
            opts.inputs.push_back({type, std::string(token)});
        } else if (token == "--") {
            positional = true;
        } else if (token == "--help" || token == "-h") {
            opts.help = true;
        } else if (token == "--version") {
            opts.version = true;
        } else if (token == "-v") {
            opts.verbose = true;
        } else if (token == "-E") {
            stage(Stage::preprocess);
        } else if (token == "-fsyntax-only") {
            stage(Stage::syntax);
        } else if (option("--emit-ast")) {
            stage(Stage::ast);
            const auto value = argument("--emit-ast");
            cephyr_ast_format format{};
            if (value == "json") format = CEPHYR_AST_JSON;
            if (value == "yaml") format = CEPHYR_AST_YAML;
            if (value == "sexp" || value == "sexpr") format = CEPHYR_AST_SEXP;
            if (!format && !value.empty())
                diag.error("cephyr.option", "unsupported AST format '" + value + "'");
            else if (opts.ast_format && format != opts.ast_format)
                diag.error("cephyr.option", "conflicting AST formats");
            else
                opts.ast_format = format;
        } else if (token == "--emit-mir") {
            stage(Stage::mir);
        } else if (token == "-S") {
            stage(Stage::assembly);
        } else if (token == "-c") {
            stage(Stage::object);
        } else if (token.starts_with("-o") && !token.starts_with("--")) {
            opts.output = argument("-o");
        } else if (token.starts_with("-x") && !token.starts_with("--")) {
            const auto value = argument("-x");
            if (auto type = language(value))
                current = *type;
            else if (!value.empty())
                diag.error("cephyr.option", "unsupported language '" + value + "'");
        } else if (option("-std")) {
            const auto value = argument("-std");
            if (value != "c11" && value != "c1x" && value != "iso9899:2011")
                diag.error("cephyr.option", "unsupported C standard '" + value + "'; use -std=c11");
        } else if (option("--config")) {
            opts.config_file = argument("--config");
        } else if (token == "--allow-native-plugins") {
            opts.allow_native_plugins = true;
        } else if (option("--lua-extension")) {
            if (auto file = argument("--lua-extension"); !file.empty()) {
                if (opts.lua_files.size() == CEPHYR_LUA_MAX_SCRIPTS)
                    diag.error("cephyr.lua.options", "too many Lua extensions");
                else
                    opts.lua_files.push_back(std::move(file));
            }
        } else if (option("--lua-env")) {
            if (auto entry = argument("--lua-env"); !entry.empty()) {
                if (opts.lua_environment.size() == CEPHYR_LUA_MAX_ENV_ENTRIES)
                    diag.error("cephyr.lua.options", "too many Lua environment entries");
                else
                    opts.lua_environment.push_back(std::move(entry));
            }
        } else if (option("--profile")) {
            opts.qml_profile = argument("--profile");
        } else if (option("--qml-pass")) {
            if (auto pass = argument("--qml-pass"); !pass.empty())
                opts.qml.passes.push_back(std::move(pass));
        } else if (option("--qml-file")) {
            if (auto file = argument("--qml-file"); !file.empty())
                opts.qml.files.emplace_back(std::move(file));
        } else if (option("--qml-fuel")) {
            const auto value = argument("--qml-fuel");
            if (!value.empty()) {
                if (auto fuel = positive_unsigned(value))
                    opts.qml.fuel = *fuel;
                else
                    diag.error("cephyr.option", "--qml-fuel must be a positive unsigned integer");
            }
        } else if (option("--qml-param")) {
            const auto param = argument("--qml-param");
            if (!param.empty()) {
                const auto dot = param.find('.');
                const auto eq = param.find('=');
                if (dot == std::string::npos || eq == std::string::npos || !dot || dot + 1 >= eq)
                    diag.error("cephyr.option", "--qml-param requires PHASE.KEY=N");
                else if (param.compare(dot + 1, eq - dot - 1, "fuel") != 0)
                    diag.error("cephyr.option", "unknown QML parameter '" + param.substr(0, eq) + "'");
                else if (auto fuel = positive_unsigned(std::string_view(param).substr(eq + 1)))
                    opts.qml.phase_fuel.insert_or_assign(param.substr(0, dot), *fuel);
                else
                    diag.error("cephyr.option", "QML phase fuel must be a positive unsigned integer");
            }
        } else if (option("--target") || token == "-target") {
            opts.target = argument(token == "-target" ? "-target" : "--target");
        } else if (option("--sysroot")) {
            opts.sysroot = argument("--sysroot");
        } else if (option("--stdlib")) {
            opts.stdlib = argument("--stdlib");
            if (opts.stdlib != "cephyr" && opts.stdlib != "system" && opts.stdlib != "none")
                diag.error("cephyr.option", "--stdlib must be cephyr, system, or none");
        } else if (option("--diagnostics-format") || option("-fdiagnostics-format")) {
            const auto value = argument(token.starts_with("--") ? "--diagnostics-format" : "-fdiagnostics-format");
            if (value == "sarif" || value == "text")
                opts.sarif = value == "sarif";
            else
                diag.error("cephyr.option", "diagnostics format must be text or sarif");
        } else if (token == "--moonstone-analyze") {
            opts.moonstone_analysis = true;
#ifndef CEPHYR_WITH_MOONSTONE_ANALYSIS
            diag.error("cephyr.moonstone.analysis", "Cephyr was built without Moonstone analysis support");
#endif
        } else if (option("--moonstone-profile")) {
            opts.profile_output = argument("--moonstone-profile");
            if (!moonstone_tools::profile_available())
                diag.error("cephyr.moonstone.profile", "Cephyr was built without Prolific profiling support");
            if (opts.profile_output == "-")
                diag.error("cephyr.moonstone.profile", "profiling requires a file, not standard output");
        } else if (token == "--sandslab" || token == "--no-sandslab") {
            opts.sandslab_enabled = token == "--sandslab";
        } else if (token == "--no-factir" || token == "--factir") {
            opts.factir_enabled = token == "--factir";
        } else if (option("--factir-smtlib")) {
            opts.factir_smtlib = argument("--factir-smtlib");
        } else if (option("--factir-facts")) {
            opts.factir_facts = argument("--factir-facts");
        } else if (token == "--no-cache" || token == "--cache") {
            opts.use_cache = token == "--cache";
        } else if (token == "-static" || token == "-dynamic") {
            opts.static_link = token == "-static";
        } else if (token == "-shared") {
            opts.shared = true;
        } else if (token == "-fPIC" || token == "-fpic" || token == "-fPIE" || token == "-fpie") {
            opts.pic = true;
        } else if (token == "-fno-PIC" || token == "-fno-pic" || token == "-fno-PIE" || token == "-fno-pie") {
            opts.pic = false;
        } else if (token == "-ffreestanding" || token == "-fhosted") {
            opts.hosted = token == "-fhosted";
        } else if (token == "-nostdinc") {
            opts.no_headers = true;
        } else if (token == "-nostdlib") {
            opts.no_start = opts.no_libraries = true;
        } else if (token == "-nodefaultlibs") {
            opts.no_libraries = true;
        } else if (token == "-nostartfiles") {
            opts.no_start = true;
        } else if (token == "-P") {
            opts.line_markers = false;
        } else if (token == "-C") {
            opts.keep_comments = true;
        } else if (token == "-trigraphs" || token == "-fno-trigraphs") {
            opts.trigraphs = token == "-trigraphs";
        } else if (token == "-Werror" || token == "-Wno-error") {
            opts.werror = token == "-Werror";
        } else if (token == "-Wall" || token == "-Wextra" || token == "-pedantic" || token == "-O0") {
            // The frontend always enables its full set of diagnostic checks;
            // no extra optimization pass is requested at -O0.
        } else if (token.starts_with("-I")) {
            opts.preprocess.include_dirs.emplace_back(argument("-I"));
        } else if (token.starts_with("-isystem")) {
            opts.preprocess.system_include_dirs.emplace_back(argument("-isystem"));
        } else if (token.starts_with("-include")) {
            opts.preprocess.forced_includes.emplace_back(argument("-include"));
        } else if (token.starts_with("-D") || token.starts_with("-U")) {
            opts.preprocess.macros.emplace_back(token[1] == 'D', argument(token.substr(0, 2)));
        } else if (token.starts_with("-L") || token.starts_with("-l")) {
            const auto value = argument(token.substr(0, 2));
            opts.inputs.push_back({Language::link_option, std::string(token.substr(0, 2)) + value});
        } else if (token.starts_with("-Wa,") || token.starts_with("-Wl,")) {
            auto args = comma_arguments(token.substr(4));
            if (!args) {
                diag.error("cephyr.option", "empty assembler or linker option");
            } else if (token[2] == 'a') {
                extend(opts.assembler_options, *args);
            } else {
                for (auto &arg : *args)
                    opts.inputs.push_back({Language::link_option, std::move(arg)});
            }
        } else if (token == "-Xassembler" || token == "-Xlinker") {
            auto value = argument(token);
            if (token == "-Xassembler")
                opts.assembler_options.push_back(std::move(value));
            else
                opts.inputs.push_back({Language::link_option, std::move(value)});
        } else {
            diag.error("cephyr.option", "unrecognized option '" + std::string(token) + "'");
        }
    }
    diag.set_warnings_as_errors(opts.werror);
    if (!units.empty() && (!input_dash || !opts.shared || opts.output != "-" ||
                           opts.stage != Stage::link || opts.help || opts.version))
        diag.error("cephyr.option", "linking translation units requires a C input operand '-', -shared and -o -");
    if (opts.help || opts.version)
        return diag.error_count() == 0;
    if (opts.stage == Stage::ast && (opts.inputs.size() != 1 || !c_input(opts.inputs.front().language)))
        diag.error("cephyr.option", "--emit-ast requires exactly one C or C11 AST input");
    if (opts.stage == Stage::preprocess && std::ranges::any_of(opts.inputs, [](const Input &input) {
            return input_ast_format(input.language) != 0;
        }))
        diag.error("cephyr.option", "-E cannot preprocess a serialized AST");
    if (opts.inputs.empty()) {
        if (!opts.verbose)
            diag.error("cephyr.option", "no input files");
    } else {
        const auto n = std::count_if(opts.inputs.begin(), opts.inputs.end(),
            [&](const Input &input) { return for_stage(input.language, opts.stage); });
        if (!n)
            diag.error("cephyr.option", "no input files for the selected stage");
        if (!opts.output.empty() && n > 1 && opts.stage != Stage::link && opts.stage != Stage::preprocess)
            diag.error("cephyr.option", "-o requires one input with this compilation stage");
    }
    if (opts.stage == Stage::syntax && !opts.output.empty())
        diag.error("cephyr.option", "-fsyntax-only does not write an output");
    if ((opts.stage == Stage::object || opts.stage == Stage::link) && opts.output == "-") {
        if (!embedding)
            diag.error("cephyr.option", "binary output to stdout is not supported");
        else if (opts.stage == Stage::link && !opts.shared)
            diag.error("cephyr.option", "capturing native output requires -c or -shared");
    }
    if (opts.shared && opts.static_link == true)
        diag.error("cephyr.option", "-shared and -static cannot be combined");
    if (opts.shared && opts.stage != Stage::link)
        diag.error("cephyr.option", "-shared requires the link stage; use -fPIC to compile an object");
    if (opts.moonstone_analysis && (opts.stage == Stage::preprocess || opts.stage == Stage::ast))
        diag.error("cephyr.moonstone.analysis", "Moonstone analysis requires C lowering");
    else if (opts.moonstone_analysis && !opts.inputs.empty() &&
             std::none_of(opts.inputs.begin(), opts.inputs.end(), [](const auto &input) {
                 return c_input(input.language);
             }))
        diag.error("cephyr.moonstone.analysis", "Moonstone analysis requires at least one C input");
    return diag.error_count() == 0;
}

bool path_exists(const fs::path &path)
{
    std::error_code ec;
    return fs::exists(path, ec);
}

// Resolve the loaded object while a relative dlopen path still has its
// original meaning. Do not depend on the embedding executable or on the
// directory from which a later request is made.
const fs::path module_directory = []() noexcept -> fs::path {
    static const char anchor = 0;
    Dl_info module{};
    try {
        if (dladdr(&anchor, &module) && module.dli_fname && *module.dli_fname) {
            std::error_code ec;
            const auto object = fs::canonical(module.dli_fname, ec);
            if (!ec)
                return object.parent_path();
        }
    } catch (...) {
        // Report a resource error on the first compilation request.
    }
    return {};
}();

std::pair<fs::path, fs::path> resources()
{
    if (module_directory == fs::path(CEPHYR_BUILD_LIBDIR))
        return {CEPHYR_SOURCE_RESOURCES, CEPHYR_SOURCE_ARCHON};
    if (module_directory.empty())
        throw std::runtime_error("cannot locate libcephyr resource directory");
    return {(module_directory / CEPHYR_LIB_RELATIVE_RESOURCES).lexically_normal(),
            (module_directory / CEPHYR_LIB_RELATIVE_ARCHON).lexically_normal()};
}

fs::path config_path(const Options &opts)
{
    if (!opts.config_file.empty())
        return opts.config_file;
    if (const char *env = std::getenv("CEPHYR_CONFIG"); env && *env)
        return env;
    return path_exists("Cephyr.toml") ? fs::path("Cephyr.toml") : fs::path{};
}

std::optional<Config> configure(const Options &opts, const cephyr_request &request, Diagnostics &diag)
{
    const auto [cephyr, archon] = resources();
    Config config = default_config(cephyr, archon);
    config.package_root = cephyr == fs::path(CEPHYR_SOURCE_RESOURCES) ?
        fs::path(CEPHYR_BUILD_PACKAGES) : cephyr / "native-packages";
    const auto config_file = config_path(opts);
    if (!config_file.empty() && !load_config(config, config_file, diag))
        return std::nullopt;
    if (request.resource_root) {
        const auto root = fs::absolute(request.resource_root);
        config.triple_dir = (root / "triples").lexically_normal();
        config.stdlib_root = (root / "stdlib").lexically_normal();
    }
    if (request.archon_root)
        config.archon_root = fs::absolute(request.archon_root).lexically_normal();
    if (!opts.target.empty() && !(opts.target == "auto" && !config.triple_script.empty())) {
        if (opts.target != "auto" && opts.target != config.target.value_or(default_target_triple))
            config.triple_script.clear();
        config.target = opts.target;
    }
    if (!opts.stdlib.empty())
        config.stdlib = opts.stdlib;
    if (!opts.sysroot.empty())
        config.sysroot = fs::absolute(opts.sysroot);
    if (opts.use_cache)
        config.cache_enabled = *opts.use_cache;
    if (opts.factir_enabled)
        config.factir_enabled = *opts.factir_enabled;
    if (opts.sandslab_enabled)
        config.sandslab_enabled = *opts.sandslab_enabled;
    if (opts.static_link)
        config.static_link = *opts.static_link;
    if (opts.shared)
        config.static_link = false;
    config.verbose = opts.verbose;
    auto &prep = config.preprocess;
    if (opts.line_markers) prep.line_markers = *opts.line_markers;
    if (opts.keep_comments) prep.keep_comments = *opts.keep_comments;
    if (opts.trigraphs) prep.trigraphs = *opts.trigraphs;
    if (opts.hosted) prep.hosted = *opts.hosted;
    prep.include_dirs.insert(prep.include_dirs.begin(),
        opts.preprocess.include_dirs.begin(), opts.preprocess.include_dirs.end());
    prep.system_include_dirs.insert(prep.system_include_dirs.begin(),
        opts.preprocess.system_include_dirs.begin(), opts.preprocess.system_include_dirs.end());
    prep.forced_includes.insert(prep.forced_includes.end(),
        opts.preprocess.forced_includes.begin(), opts.preprocess.forced_includes.end());
    prep.macros.insert(prep.macros.end(), opts.preprocess.macros.begin(), opts.preprocess.macros.end());
    return config;
}

PreprocessOptions preprocessor_options(const Options &opts, const Config &config,
                                          const Target &target, const NativeLoweringProvider *provider,
                                          bool custom_model, bool native_scalar_fp,
                                          bool native_rv64_lp64, bool assembler)
{
    auto result = config.preprocess;
    result.assembler = assembler;
    result.plain_char_signed = provider && provider->plain_char_signed;
    result.wchar_signed = !provider || provider->wchar_signed;
    std::vector<std::pair<bool, std::string>> builtins{
        {true, "__CEPHYR__=1"}, {true, "__CEPHYR_HAS_VARIADIC_CALLS=1"},
        {true, "__LP64__=1"}, {true, "_LP64=1"},
    };
    if (native_scalar_fp && !assembler)
        builtins.emplace_back(true, "__CEPHYR_HAS_SCALAR_FP=1");
    if (!assembler && (native_rv64_lp64 || (native_scalar_fp && provider->architecture == "AArch64")))
        builtins.emplace_back(true, "__CEPHYR_HAS_BINARY128_ABI=1");
    if (native_rv64_lp64 && !assembler) {
        builtins.emplace_back(true, "__CEPHYR_HAS_NATIVE_DIVISION_API=1");
        builtins.emplace_back(true, "__CEPHYR_HAS_RV64_STDARG=1");
    }
    if (native_scalar_fp && provider->architecture == "AMD64" && !assembler) {
        builtins.emplace_back(true, "__CEPHYR_HAS_AMD64_STDARG=1");
        builtins.emplace_back(true, "__CEPHYR_HAS_X87_ABI=1");
    }
    if (native_scalar_fp && provider->architecture == "AArch64" && !assembler)
        builtins.emplace_back(true, "__CEPHYR_HAS_AARCH64_STDARG=1");
    if (provider && provider->architecture == "RISCV64IM")
        for (const auto *macro : {"__riscv=1", "__riscv_xlen=64", "__riscv_mul=1", "__riscv_div=1",
                                  "__riscv_float_abi_soft=1", "__CHAR_UNSIGNED__=1"})
            builtins.emplace_back(true, macro);
    if (provider && provider->architecture == "AMD64")
        for (const auto *macro : {"__x86_64__=1", "__x86_64=1", "__amd64__=1", "__amd64=1"})
            builtins.emplace_back(true, macro);
    if (provider && provider->architecture == "AArch64")
        for (const auto *macro : {"__aarch64__=1", "__AARCH64EL__=1", "__ARM_ARCH=8",
                                  "__ARM_ARCH_ISA_A64=1", "__ARM_PCS_AAPCS64=1", "__CHAR_UNSIGNED__=1"})
            builtins.emplace_back(true, macro);
    if (custom_model)
        builtins.emplace_back(true, "__CHAR_UNSIGNED__=1");
    if (target.is_linux) {
        builtins.emplace_back(true, "__linux__=1");
        builtins.emplace_back(true, "__unix__=1");
    }
    if (assembler)
        builtins.emplace_back(true, "__ASSEMBLER__=1");
    result.macros.insert(result.macros.begin(), builtins.begin(), builtins.end());
    if (!opts.no_headers) {
        if (config.stdlib == "cephyr" && !config.stdlib_root.empty()) {
            result.system_include_dirs.push_back(config.stdlib_root / "include");
        } else if (config.stdlib == "system" && !config.sysroot.empty()) {
            const auto include = config.sysroot / "usr/include";
            if (path_exists(include / target.triple))
                result.system_include_dirs.push_back(include / target.triple);
            result.system_include_dirs.push_back(include);
        }
    }
    return result;
}

std::optional<std::string> read_input(const Input &input, const Invocation &invocation,
                                      Diagnostics &diag)
{
    const bool serialized = input_ast_format(input.language);
    const std::size_t max_input = serialized ? syntax::max_document_bytes : detail::max_source_size;
    const char *limit_message = serialized ? "input exceeds the 64 MiB syntax document limit" :
                                             "input exceeds the 16 MiB source limit";
    const std::string name = input.unit ? input.name() : input.argument == "-" ? "<stdin>" : input.argument;
    if (input.argument == "-" && !invocation.cli_input) {
        const auto &request = invocation.request;
        const auto size = input.unit ? input.unit->source.size() : request.stdin_size;
        if (size > max_input) {
            diag.error("cephyr.limit", limit_message, Location{name});
            return std::nullopt;
        }
        const std::string_view source = input.unit ? std::string_view(input.unit->source) :
            std::string_view(request.stdin_data ? request.stdin_data : "", size);
        if (source.find('\0') != std::string_view::npos) {
            diag.error("cephyr.input", "input contains a NUL byte", Location{name});
            return std::nullopt;
        }
        return std::string(source);
    }
    std::ifstream file;
    std::istream *stream = invocation.cli_input;
    if (input.argument != "-") {
        file.open(input.argument, std::ios::binary);
        stream = &file;
    }
    if (!*stream) {
        diag.error("cephyr.input", "cannot open input", Location{name});
        return std::nullopt;
    }
    std::string result;
    char buffer[32768];
    while (stream->read(buffer, sizeof(buffer)) || stream->gcount()) {
        const auto count = static_cast<std::size_t>(stream->gcount());
        if (count > max_input - result.size()) {
            diag.error("cephyr.limit", limit_message, Location{name});
            return std::nullopt;
        }
        result.append(buffer, count);
    }
    if (!stream->eof() || stream->bad() || result.find('\0') != std::string::npos) {
        diag.error("cephyr.input", "input is unreadable or contains a NUL byte", Location{name});
        return std::nullopt;
    }
    return result;
}

bool same_file(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (fs::equivalent(a, b, ec))
        return true;
    return fs::weakly_canonical(a) == fs::weakly_canonical(b);
}

bool can_publish(const fs::path &path, const Options &opts, Diagnostics &diag)
{
    if (path == "-")
        return true;
    for (const auto &input : opts.inputs) {
        if (input.language != Language::link_option && input.argument != "-" &&
            same_file(path, input.argument)) {
            diag.error("cephyr.output", "output would overwrite an input file", Location{path.string()});
            return false;
        }
    }
    for (const auto &file : opts.qml.files) {
        if (same_file(path, file)) {
            diag.error("cephyr.output", "output would overwrite a QML rule file", Location{path.string()});
            return false;
        }
    }
    return true;
}

bool publish_stdout(std::string_view text, Invocation &invocation, Diagnostics &diag)
{
    if (!invocation.cli_output) {
        invocation.result.output.append(text);
        return true;
    }
    auto &output = *invocation.cli_output;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output)
        diag.error("cephyr.output", "cannot write standard output");
    return bool(output);
}

bool publish_text(const fs::path &output, std::string_view text, const Options &opts,
                  Invocation &invocation, Diagnostics &diag)
{
    if (output == "-")
        return publish_stdout(text, invocation, diag);
    if (!can_publish(output, opts, diag))
        return false;
    TempDirectory staging(output.has_parent_path() ? output.parent_path() : fs::path("."), ".cephyr-out-XXXXXX");
    const auto file = staging.path / "result";
    save(file, text);
    fs::rename(file, output);
    return true;
}

bool publish_file(const fs::path &output, const fs::path &file, const Options &opts, Diagnostics &diag)
{
    if (!can_publish(output, opts, diag))
        return false;
    TempDirectory staging(output.has_parent_path() ? output.parent_path() : fs::path("."), ".cephyr-out-XXXXXX");
    const auto stage = staging.path / "result";
    fs::copy_file(file, stage);
    fs::rename(stage, output);
    return true;
}

bool publish_native(const fs::path &output, const fs::path &file, const Options &opts,
                    Invocation &invocation, Diagnostics &diag, const MemoryFile *memory = nullptr)
{
    if (output != "-")
        return publish_file(output, file, opts, diag);

    // Only an embedding request can select binary capture. Keep the result
    // empty until the complete artifact has been read from the tool workspace.
    constexpr std::size_t limit = 128u * 1024u * 1024u;
    std::error_code error;
    if (!fs::is_regular_file(file, error)) {
        diag.error("cephyr.output", "configured tool did not produce a regular native artifact");
        return false;
    }
    const auto size = fs::file_size(file, error);
    if (error || size == 0) {
        diag.error("cephyr.output", "cannot read native artifact from the configured tool");
        return false;
    }
    if (size > limit) {
        diag.error("cephyr.limit", "captured native artifact exceeds 128 MiB");
        return false;
    }
    if (memory) {
        invocation.result.output = memory->read(static_cast<std::size_t>(size));
    } else {
        std::ifstream stream(file, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
            diag.error("cephyr.output", "cannot read native artifact from the configured tool");
            return false;
        }
        invocation.result.output = std::move(bytes);
    }
    invocation.result.loadable_output = opts.shared;
    return true;
}

fs::path output_name(const Input &input, const Options &opts)
{
    if (!opts.output.empty())
        return opts.output;
    if (opts.stage == Stage::preprocess)
        return "-";
    auto name = input.argument == "-" ? fs::path("stdin") : fs::path(input.argument).filename();
    name.replace_extension(opts.stage == Stage::assembly ? ".s" : opts.stage == Stage::mir ? ".mir" : ".o");
    return name;
}

bool can_publish_profile(const Options &opts, Diagnostics &diag)
{
    const auto &profile = opts.profile_output;
    if (!can_publish(profile, opts, diag))
        return false;
    const auto config = config_path(opts);
    if (!config.empty() && same_file(profile, config)) {
        diag.error("cephyr.moonstone.profile", "profile would overwrite the configuration file",
                   Location{profile.string()});
        return false;
    }
    const auto conflicts = [&](const fs::path &output) {
        return !output.empty() && output != "-" && same_file(profile, output);
    };
    bool same_output = conflicts(opts.output);
    if (opts.stage == Stage::link && opts.output.empty())
        same_output = conflicts("a.out");
    if (opts.output.empty() && (opts.stage == Stage::object ||
        opts.stage == Stage::assembly || opts.stage == Stage::mir)) {
        for (const auto &input : opts.inputs) {
            if (for_stage(input.language, opts.stage) &&
                !(opts.stage == Stage::assembly && input.language == Language::assembly) &&
                conflicts(output_name(input, opts)))
                same_output = true;
        }
    }
    if (same_output)
        diag.error("cephyr.moonstone.profile", "profile and compiler output must be different files",
                   Location{profile.string()});
    return !same_output;
}

bool check_factir_outputs(const Options &opts, const Config &config,
                          const std::set<fs::path> &destinations, Diagnostics &diag)
{
    if (opts.factir_smtlib.empty() && opts.factir_facts.empty())
        return true;
    if (!config.factir_enabled || opts.stage == Stage::syntax || opts.stage == Stage::preprocess ||
        opts.stage == Stage::ast ||
        std::count_if(opts.inputs.begin(), opts.inputs.end(),
                      [](const Input &input) { return c_input(input.language); }) != 1) {
        diag.error("cephyr.factir.output", "FactIR artifacts require enabled FactIR and exactly one C input being lowered");
        return false;
    }
    const auto identity = [](const fs::path &path) {
        return fs::weakly_canonical(fs::absolute(path));
    };
    std::set<fs::path> reserved;
    for (const auto &path : destinations)
        reserved.insert(identity(path));
    for (const auto &path : {config_path(opts), opts.output, opts.profile_output})
        if (!path.empty() && path != "-")
            reserved.insert(identity(path));
    if (opts.stage == Stage::link && opts.output.empty())
        reserved.insert(identity("a.out"));
    for (const auto &path : {opts.factir_smtlib, opts.factir_facts}) {
        if (path.empty())
            continue;
        const auto parent = path.has_parent_path() ? path.parent_path() : fs::path(".");
        if (path == "-" || !fs::is_directory(parent) ||
            fs::symlink_status(path).type() != fs::file_type::not_found ||
            !reserved.insert(identity(path)).second ||
            !can_publish(path, opts, diag)) {
            diag.error("cephyr.factir.output", "FactIR artifacts require distinct, new output files",
                       Location{path.string()});
            return false;
        }
    }
    return true;
}

bool publish_factir_artifact(const fs::path &path, std::string_view bytes, Diagnostics &diag)
{
    if (path.empty())
        return true;
    TempDirectory staging(path.has_parent_path() ? path.parent_path() : fs::path("."), ".cephyr-factir-XXXXXX");
    const auto file = staging.path / "artifact";
    save(file, bytes);
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write);
    // Atomic create-only publication, including races with another invocation.
    std::error_code error;
    fs::create_hard_link(file, path, error);
    if (error)
        diag.error("cephyr.factir.output", "cannot publish a new file: " + error.message(), Location{path.string()});
    return !error;
}

template <class Action>
decltype(auto) measured(MoonstoneProfile *profile, std::string_view stage,
                        std::string_view input, Action &&action)
{
    if (profile) {
        const auto span = profile->measure(stage, input);
        return std::forward<Action>(action)();
    }
    return std::forward<Action>(action)();
}

void sysroot(Command &args, const Config &config)
{
    if (!config.sysroot.empty())
        args.push_back("--sysroot=" + config.sysroot.string());
}

bool execute(const Command &args, const Options &opts, Diagnostics &diag,
             std::span<const int> descriptors = {}, int stdout_fd = -1)
{
    if (opts.verbose) {
        std::string line;
        for (const auto &arg : args) {
            if (!line.empty()) line += ' ';
            line += arg;
        }
        diag.note("cephyr.command", line);
    }
    const auto before = diag.error_count();
    return run_process(args, diag, {}, descriptors, stdout_fd) == 0 && diag.error_count() == before;
}

bool add_qml_bundle(qml::Pipeline &pipeline, const fs::path &path, Diagnostics &diag)
{
    const auto name = path.string();
    constexpr auto max_size = 16u * 1024u * 1024u;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max_size) {
        diag.error("cephyr.qml.bundle", ec ? "cannot read compiled QML rules: " + ec.message() :
                   "compiled QML rules exceed the 16 MiB limit", Location{name});
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!file.read(contents.data(), static_cast<std::streamsize>(size)) ||
        file.peek() != std::char_traits<char>::eof() || file.bad()) {
        diag.error("cephyr.qml.bundle", "cannot read compiled QML rules", Location{name});
        return false;
    }
    return pipeline.add_compiled(contents, diag, name);
}

bool configure_qml(Options &opts, const Config &config,
                   std::optional<qml::Pipeline> &pipeline, Diagnostics &diag)
{
    const auto name = opts.qml_profile.empty() ? config.qml_profile.value_or("") : opts.qml_profile;
    if (!name.empty()) {
        const auto selected = config.qml_profiles.find(name);
        if (selected == config.qml_profiles.end()) {
            diag.error("cephyr.qml.profile", "unknown QML profile '" + name + "'",
                       Location{config_path(opts).string()});
            return false;
        }
        auto effective = selected->second;
        extend(effective.passes, opts.qml.passes);
        effective.files.insert(effective.files.end(), opts.qml.files.begin(), opts.qml.files.end());
        if (opts.qml.fuel)
            effective.fuel = opts.qml.fuel;
        for (const auto &[phase, fuel] : opts.qml.phase_fuel)
            effective.phase_fuel.insert_or_assign(phase, fuel);
        opts.qml = std::move(effective);
    }
    const auto &[passes, files, fuel, phase_fuel] = opts.qml;
    for (const auto &parameter : phase_fuel) {
        const auto &phase = parameter.first;
        if (std::find(passes.begin(), passes.end(), phase) == passes.end()) {
            diag.error("cephyr.qml.profile", "parameter specified for unselected QML pass '" + phase + "'");
            return false;
        }
    }
    if (passes.empty()) {
        if (files.empty() && !fuel)
            return true;
        diag.error("cephyr.qml.profile", "select at least one QML pass when supplying rule files or fuel");
        return false;
    }

    if (!opts.profile_output.empty() && !can_publish(opts.profile_output, opts, diag))
        return false;
    const auto resource_root = resources().first;
    const bool build_tree = resource_root == fs::path(CEPHYR_SOURCE_RESOURCES);
    const auto assets = build_tree ? fs::path(CEPHYR_BUILD_QML) : resource_root / "qml";
    pipeline.emplace();
    if (!add_qml_bundle(*pipeline, assets / "builtin.qmb", diag))
        return false;
    if (!files.empty()) {
        const auto compiler = assets / "compile_rules.py";
        const auto frontend = assets / "qml_frontend.py";
        if (!path_exists(compiler) || !path_exists(frontend)) {
            diag.error("cephyr.qml.bundle", "QML rule compiler or generated frontend is missing",
                       Location{assets.string()});
            return false;
        }
        for (const auto &file : files) {
            std::error_code error;
            if (!fs::is_regular_file(file, error)) {
                diag.error("cephyr.qml.rules", "QML rule input is not a readable regular file",
                           Location{file.string()});
                return false;
            }
        }
        TempDirectory directory(fs::absolute(fs::temp_directory_path()), "cephyr-qml-XXXXXX");
        const auto bundle = directory.path / "rules.qmb";
        Command command{build_tree ? CEPHYR_BUILD_PYTHON : "python3", "-I", "-B", compiler.string(),
                        "--frontend", frontend.string(), "--output", bundle.string(), "--"};
        for (const auto &file : files)
            command.push_back(fs::absolute(file).string());
        if (!execute(command, opts, diag) || !add_qml_bundle(*pipeline, bundle, diag))
            return false;
    }
    return pipeline->configure(passes, fuel.value_or(4096), diag, phase_fuel);
}

bool verify_mir(std::string_view mir, Diagnostics &diag, std::string_view name,
                std::string_view code)
{
    ArchonBackendDiagnostic verification{};
    if (cephyr_verify_mir(mir.data(), mir.size(), &verification) == ARCHON_BACKEND_OK)
        return true;
    diag.error(code, verification.message[0] ? verification.message :
               "Archon rejected MachineIR", Location{std::string(name)});
    return false;
}

bool assemble(const Config &config, const Target &target, const Options &opts,
              const fs::path &input, const fs::path &output, Diagnostics &diag,
              std::span<const int> descriptors = {})
{
    Command cmd = config.assembler.empty() ? target.assembler : config.assembler;
    if (config.assembler.empty())
        extend(cmd, target.assemble_flags);
    extend(cmd, config.assemble_flags);
    if (config.assembler_style == "driver") {
        sysroot(cmd, config);
        extend(cmd, {"-c", "-x", "assembler"});
        for (const auto &arg : opts.assembler_options)
            extend(cmd, {"-Xassembler", arg});
    } else {
        extend(cmd, opts.assembler_options);
    }
    extend(cmd, {input.string(), "-o", output.string()});
    return execute(cmd, opts, diag, descriptors);
}

struct Runtime {
    fs::path startup, library;
};

struct ToolArtifact {
    std::optional<MemoryFile> memory;
    fs::path path;

    ToolArtifact(bool anonymous, const char *label, const auto &workspace)
    {
        if (anonymous) {
            memory.emplace(label);
            if (memory->fd() >= 0)
                path = memory->path();
            else
                memory.reset();
        }
        if (!memory)
            path = workspace() / label;
    }

    void write(std::string_view bytes) const
    {
        if (memory)
            memory->write(bytes);
        else
            save(path, bytes);
    }

    void inherit(std::vector<int> &descriptors) const
    {
        if (memory)
            descriptors.push_back(memory->fd());
    }
};

bool anonymous_assembly(const Config &config, const Target &target, const Options &opts)
{
    return config.assembler.empty() && config.triple_script.empty() &&
        !target.assembler.empty() && fs::path(target.assembler.front()).filename() == "sandstone-as" &&
        config.assemble_flags.empty() && opts.assembler_options.empty();
}

bool compiler_driver_link(const Config &config)
{
    if (config.stdlib == "system")
        return true;
    if (config.linker.empty() || is_raw_linker_command(config.linker))
        return false;
    // Common compiler-driver commands, including prefixed tools and tools
    // passed through a wrapper. Otherwise use the raw ld-compatible protocol.
    return std::any_of(config.linker.begin(), config.linker.end(), [](const std::string &arg) {
        const auto name = fs::path(arg).filename().string();
        return name == "cc" || name == "gcc" || name == "clang" ||
            name.ends_with("-gcc") || name.ends_with("-clang") ||
            name.starts_with("clang-") || name.starts_with("gcc-");
    });
}

std::optional<Runtime> packaged_runtime(const Options &opts, const Config &config,
                                        const Target &target, Diagnostics &diag)
{
    Runtime result;
    const auto directory = config.package_root / target.architecture;
    if (!opts.no_start) result.startup = directory / "crt0.o";
    if (!opts.no_libraries) result.library = directory / "libc.a";
    for (const auto &path : {result.startup, result.library}) {
        if (!path.empty() && !fs::is_regular_file(path)) {
            diag.error("cephyr.stdlib", "missing packaged target runtime: " + path.string());
            return std::nullopt;
        }
    }
    return result;
}

bool link(const Config &config, const Target &target, const Options &opts,
          Command inputs, std::vector<int> descriptors, const auto &workspace,
          Invocation &invocation, Diagnostics &diag)
{
    auto output = opts.output.empty() ? fs::path("a.out") : opts.output;
    if (!can_publish(output, opts, diag))
        return false;
    Command cmd = config.linker.empty() ? target.linker : config.linker;
    if (config.linker.empty())
        extend(cmd, target.link_flags);
    extend(cmd, config.link_flags);
    sysroot(cmd, config);
    if (opts.shared)
        cmd.push_back("-shared");
    else if (config.static_link)
        cmd.push_back("-static");
    const bool driver = compiler_driver_link(config);
    const auto linker_name = cmd.empty() ? std::string{} : fs::path(cmd.front()).filename().string();
    const bool lld = !driver && linker_name == "ld.lld";
    const bool qld = !driver && linker_name == "qobjld";
    if (!opts.shared && !qld)
        cmd.push_back(driver ? "-Wl,--fatal-warnings" : "--fatal-warnings");

    Runtime runtime;
    if (config.stdlib == "cephyr" && !(opts.no_start && opts.no_libraries)) {
        auto built = packaged_runtime(opts, config, target, diag);
        if (!built)
            return false;
        runtime = std::move(*built);
    } else if (config.stdlib == "system") {
        if (opts.no_start && opts.no_libraries) cmd.push_back("-nostdlib");
        else if (opts.no_start) cmd.push_back("-nostartfiles");
        else if (opts.no_libraries) cmd.push_back("-nodefaultlibs");
    }
    if (driver && config.stdlib != "system")
        cmd.push_back("-nostdlib"); // prevent implicit injection of a different CRT/libc
    if (!runtime.startup.empty())
        cmd.push_back(runtime.startup.string());
    extend(cmd, inputs);
    std::optional<ToolArtifact> bridge_source, bridge_object;
    const bool host_bindings = invocation.symbols && !invocation.symbols->empty();
    if (host_bindings) {
#if defined(__x86_64__) && !defined(__ILP32__)
        const bool host_target = target.is_linux && target.architecture == "AMD64" &&
            target.abi == "SysVAMD64";
#else
        const bool host_target = false;
#endif
        if (!host_target || (!lld && !qld) || opts.pic == false) {
            diag.error("cephyr.host_symbol", "host registration requires AMD64 Linux host and target, PIC, and a direct qobjld or ld.lld linker");
            return false;
        }
        std::ostringstream bridge;
        bridge.imbue(std::locale::classic());
        bridge << ".text\n";
        // Change only this link's references. LLD can bind data through a
        // hidden absolute symbol if GOT-to-PC-relative relaxation is disabled.
        // A function trampoline tail-jumps with all ABI arguments intact
        // (including %al for variadics). Neither kind is exported by the DSO.
        if (lld) cmd.emplace_back("--no-relax");
        for (const auto &[name, symbol] : *invocation.symbols) {
            const auto wrapped = lld ? "__wrap_" + name : name;
            const auto address = reinterpret_cast<std::uintptr_t>(symbol.address);
            if (lld) cmd.emplace_back("--wrap=" + name);
            bridge << ".globl " << wrapped << "\n.hidden " << wrapped << '\n';
            if (symbol.kind == CEPHYR_HOST_FUNCTION) {
                bridge << ".type " << wrapped << ",@function\n" << wrapped
                       << ":\n.byte 0xf3,0x0f,0x1e,0xfa\nmovabs $0x"
                       << std::hex << address << ", %r11\njmp *%r11\n";
            } else {
                bridge << ".set " << wrapped << ", 0x" << std::hex << address << '\n';
            }
        }
        bridge << ".section .note.GNU-stack,\"\",@progbits\n";
        const bool anonymous = anonymous_assembly(config, target, opts);
        bridge_source.emplace(anonymous, "host-symbols.s", workspace);
        bridge_object.emplace(anonymous, "host-symbols.o", workspace);
        bridge_source->write(std::move(bridge).str());
        std::vector<int> assemble_fds;
        bridge_source->inherit(assemble_fds);
        bridge_object->inherit(assemble_fds);
        if (!assemble(config, target, opts, bridge_source->path, bridge_object->path, diag, assemble_fds))
            return false;
        bridge_object->inherit(descriptors);
        cmd.push_back(bridge_object->path.string());
    }
    if (!runtime.library.empty())
        cmd.push_back(runtime.library.string());
    // LLD writes via rename for named output. Its stdout mode writes directly
    // into our anonymous file instead; stderr remains on the diagnostic pipe.
    const bool passthrough = std::any_of(opts.inputs.begin(), opts.inputs.end(), [](const Input &input) {
        return input.language == Language::link_option;
    });
    const bool anonymous = (lld || qld) && config.linker.empty() && config.link_flags.empty() &&
        config.triple_script.empty() &&
        output == "-" && !passthrough;
    const ToolArtifact temporary(anonymous, "link-result", workspace);
    if (qld) temporary.inherit(descriptors);
    extend(cmd, {"-o", temporary.memory && lld ? "-" : temporary.path.string()});
    if (!execute(cmd, opts, diag, descriptors, temporary.memory && lld ? temporary.memory->fd() : -1) ||
        !publish_native(output, temporary.path, opts, invocation, diag,
                        temporary.memory ? &*temporary.memory : nullptr))
        return false;
    if (host_bindings)
        invocation.result.host_binding_process = getpid();
    return true;
}

bool check_auto_tools(const Options &opts, const Config &config, const Target &target,
                      Diagnostics &diag)
{
    if (opts.stage == Stage::preprocess || opts.stage == Stage::syntax ||
        opts.stage == Stage::ast || opts.stage == Stage::mir)
        return true;
    const auto check = [&](std::string_view role, const Command &command) {
        return check_auto_tool(target, role, command, diag);
    };
    const bool has_source = std::any_of(opts.inputs.begin(), opts.inputs.end(),
                                  [](const Input &input) { return source_input(input.language); });
    if (has_source && (opts.stage == Stage::object || opts.stage == Stage::link) &&
        !check("a target assembler", config.assembler.empty() ? target.assembler : config.assembler))
        return false;
    if (opts.stage != Stage::link)
        return true;
    return check("a target linker", config.linker.empty() ? target.linker : config.linker);
}

const cephyr_lua_options *lua_options(const cephyr_request &request);
bool load_lua_extensions(LuaExtensions &extensions, const cephyr_request &request,
                         const Options &opts, const ExtensionConfig &config,
                         const cephyr_sdk *native_sdk, Diagnostics &diag);

bool compile(Options &opts, Invocation &invocation, SdkPipeline &request_sdk, Diagnostics &diag,
             MoonstoneProfile *profile)
{
    detail::MoonstoneAnalysis *analysis = nullptr;
#ifdef CEPHYR_WITH_MOONSTONE_ANALYSIS
    std::optional<detail::MoonstoneAnalysis> analysis_storage;
    if (opts.moonstone_analysis)
        analysis = &analysis_storage.emplace(diag);
#endif
    auto loaded = measured(profile, "configure", {}, [&] {
        return configure(opts, invocation.request, diag);
    });
    if (!loaded)
        return false;
    auto &config = *loaded;
    const auto &request = invocation.request;
    const cephyr_sdk *native_sdk = request.sdk;
    std::optional<NativePlugins> plugins;
    std::optional<LuaExtensions> extensions;
    std::optional<SdkPipeline> merged;
    if (!config.extensions.native.empty()) {
        if (!opts.allow_native_plugins) {
            diag.error("cephyr.plugin.permission",
                       "configured native plugins require explicit --allow-native-plugins");
            return false;
        }
        if (!plugins.emplace().load(config.extensions.native, native_sdk, diag))
            return false;
        native_sdk = plugins->sdk();
    }
    if (lua_options(request) || !config.extensions.lua.empty() ||
        !opts.lua_files.empty() || !opts.lua_environment.empty()) {
        if (!load_lua_extensions(extensions.emplace(), request, opts, config.extensions, native_sdk, diag))
            return false;
        native_sdk = extensions->sdk();
    }
    if ((plugins || extensions) && !merged.emplace(native_sdk).validate(diag))
        return false;
    auto &sdk = merged ? *merged : request_sdk;
    std::optional<qml::Pipeline> qml_pipeline;
    if (opts.stage != Stage::preprocess && opts.stage != Stage::syntax && opts.stage != Stage::ast &&
        (!opts.qml_profile.empty() || config.qml_profile || !opts.qml.passes.empty() ||
         !opts.qml.files.empty() || opts.qml.fuel || !opts.qml.phase_fuel.empty()) &&
        std::any_of(opts.inputs.begin(), opts.inputs.end(),
                    [](const Input &input) { return c_input(input.language); })) {
        if (!measured(profile, "qml_configure", {}, [&] {
            return configure_qml(opts, config, qml_pipeline, diag);
        }))
            return false;
    }
    const bool automatic = config.target == "auto" && config.triple_script.empty();
    std::optional<Target> target;
    if (automatic) {
        const auto host = detect_host(diag);
        if (!host)
            return false;
        target = load_host_target(*host, config, host_target_mappings(), diag);
    } else {
        const auto triple = config.target == "auto" ? "" :
            config.target.value_or(config.triple_script.empty() ? default_target_triple : "");
        target = load_target(triple, config.triple_dir, config.triple_script, config.archon_root, diag);
    }
    if (!target)
        return false;
    // Package references in the target bank resolve beside this loaded copy
    // of libcephyr. Explicit [tools] overrides are never rewritten.
    if (target->assembler.front() == "@package/sandstone-as") {
        target->assembler.front() = (config.package_root / "tools/sandstone-as").string();
        if (config.assembler.empty()) config.assembler_style = "as";
    }
    for (auto &arg : target->assemble_flags) {
        if (arg.starts_with("@bank/") && fs::path(arg.substr(6)).filename() == arg.substr(6))
            arg = (config.package_root / "banks" / arg.substr(6)).string();
    }
    if (target->linker.front() == "@package/qobjld")
        target->linker.front() = (config.package_root / "tools/qobjld").string();
    const auto *provider = native_lowering_provider(target->architecture, target->abi);
    if (provider) {
        std::error_code error;
        if (!fs::equivalent(target->specification, config.archon_root / provider->stdspec, error))
            provider = nullptr;
    }
    const bool sdk_backend = sdk.has_backend(*target);
    const bool native_rv64_lp64 = provider && !sdk_backend &&
        provider->architecture == "RISCV64IM" && provider->abi == "LP64";
    const bool native_scalar_fp = provider && !sdk_backend &&
        (provider->architecture == "AMD64" || provider->architecture == "AArch64");
    // An exact SDK backend for an explicitly selected target may opt into
    // Cephyr's generic little-endian LP64 frontend, not a built-in ISA/ABI.
    const bool custom_model = !provider && !automatic && sdk_backend && target->abi == "LP64";
    if (opts.stage == Stage::link) {
        if (config.stdlib == "cephyr" && !(opts.no_start && opts.no_libraries) &&
            (!target->is_linux || !provider || !config.static_link)) {
            diag.error("cephyr.stdlib", "Cephyr's bundled startup and libc require a supported static Linux target (AMD64, AArch64, or RV64IM); for shared or dynamic links use --stdlib=system or -nostdlib");
            return false;
        }
        if (config.stdlib == "system" &&
            (config.linker.empty() || is_raw_linker_command(config.linker))) {
            diag.error("cephyr.stdlib", "--stdlib=system requires a target compiler driver in [tools].linker");
            return false;
        }
    }
    if (automatic && !check_auto_tools(opts, config, *target, diag))
        return false;
    if (opts.verbose)
        diag.note("cephyr.target", target->triple + " (" + target->architecture + "/" + target->abi + ")" +
                  (automatic ? " [auto]" : ""));

    std::set<fs::path> destinations;
    if (opts.stage == Stage::object || opts.stage == Stage::assembly || opts.stage == Stage::mir) {
        for (const auto &input : opts.inputs) {
            if (!for_stage(input.language, opts.stage) ||
                (opts.stage == Stage::assembly && input.language == Language::assembly && opts.output.empty()))
                continue;
            const auto output = output_name(input, opts);
            if (!destinations.insert(fs::weakly_canonical(output)).second) {
                diag.error("cephyr.output", "multiple inputs would produce the same output", Location{output.string()});
                return false;
            }
            if (!can_publish(output, opts, diag))
                return false;
        }
    }

    if (!check_factir_outputs(opts, config, destinations, diag))
        return false;
    if (sdk_backend && opts.stage != Stage::mir &&
        (!opts.factir_smtlib.empty() || !opts.factir_facts.empty())) {
        diag.error("cephyr.factir.output", "FactIR does not run before a custom SDK backend");
        return false;
    }
    std::optional<factir::Result> exported_facts;
    std::optional<TempDirectory> workspace;
    const auto temporary = [&]() -> const fs::path & {
        if (!workspace)
            workspace.emplace(fs::absolute(fs::temp_directory_path()), "cephyr-XXXXXX");
        return workspace->path;
    };
    std::optional<Backend> backend;
    auto preprocessor = preprocessor_options(opts, config, *target, provider,
                                            custom_model, native_scalar_fp, native_rv64_lp64, false);
    preprocessor.gcc_pragma_names = sdk.directive_names(*target);
    preprocessor.directive = [&](std::string_view name, std::string_view arguments,
                                 const Location &where, std::optional<std::string> &replacement) {
        return sdk.directive(name, arguments, *target, where, diag, replacement);
    };
    const auto assembler_preprocessor = preprocessor_options(opts, config, *target, provider,
                                                            custom_model, native_scalar_fp, native_rv64_lp64, true);
    Command link_inputs;
    std::vector<MemoryFile> link_files;
    std::vector<int> link_fds;
    std::string preprocessed_output;
    std::size_t ordinal = 0;
    for (const auto &input : opts.inputs) {
        if (!for_stage(input.language, opts.stage)) {
            diag.warning("cephyr.input", "input unused during the selected stage", Location{input.argument});
            continue;
        }
        if (!source_input(input.language)) {
            if (input.language == Language::link_option) {
                if (input.argument == "-o" || input.argument.starts_with("--output") ||
                    input.argument.starts_with("-o") || input.argument == "--" || input.argument.starts_with('@')) {
                    diag.error("cephyr.option", "link output, response files, and argument termination may not be passed through");
                    return false;
                }
                if (compiler_driver_link(config) && !input.argument.starts_with("-L") &&
                    !input.argument.starts_with("-l") && !input.argument.starts_with("-Wl,"))
                    extend(link_inputs, {"-Xlinker", input.argument});
                else
                    link_inputs.push_back(input.argument);
            } else {
                const auto path = fs::absolute(input.argument);
                if (!object_input(path)) {
                    diag.error("cephyr.input", "unrecognized input suffix; select the source language with -x or pass a linker script with -Wl,-T,FILE",
                               Location{input.argument});
                    return false;
                }
                if (!path_exists(path)) {
                    diag.error("cephyr.input", "link input does not exist", Location{input.argument});
                    return false;
                }
                link_inputs.push_back(path.string());
            }
            continue;
        }

        if (opts.stage == Stage::assembly && input.language == Language::assembly && opts.output.empty() &&
            input.argument != "-") {
            if (!path_exists(input.argument)) {
                diag.error("cephyr.input", "assembler input does not exist", Location{input.argument});
                return false;
            }
            continue;
        }
        const auto name = input.unit ? input.name() : input.argument == "-" ? "<stdin>" : input.argument;
        auto source = measured(profile, "read_source", name, [&] {
            return read_input(input, invocation, diag);
        });
        if (!source)
            return false;
        if (input.language == Language::c && !measured(profile, "sdk_desugar", name, [&] {
            return sdk.desugar(*source, *target, name, diag);
        }))
            return false;
        std::optional<Preprocessed> prep;
        std::optional<ParsedUnit> parsed;
        if (const auto format = input_ast_format(input.language)) {
            auto unit = measured(profile, "read_ast", name, [&] {
                return syntax::read(format, *source, name, diag);
            });
            if (!unit)
                return false;
            prep = std::move(unit->source);
            parsed = std::move(unit->parsed);
        } else if (input.language == Language::preprocessed) {
            prep = measured(profile, "read_preprocessed", name, [&] {
                return read_preprocessed(*source, name, diag);
            });
        } else if (input.language == Language::c || input.language == Language::assembly_cpp) {
            prep = measured(profile, "preprocess", name, [&] {
                return preprocess(*source, name,
                                  input.language == Language::c ? preprocessor : assembler_preprocessor, diag);
            });
        }
        const bool expects_preprocessor = input.language != Language::assembly;
        if ((expects_preprocessor && !prep) || diag.error_count())
            return false;
        if (opts.stage == Stage::ast) {
            if (!parsed)
                parsed = measured(profile, "parse", name, [&] { return parse(*prep, diag); });
            if (!parsed || diag.error_count())
                return false;
            const auto text = measured(profile, "emit_ast", name, [&] {
                return syntax::emit(opts.ast_format, *parsed, *prep, diag);
            });
            return text && publish_text(opts.output.empty() ? fs::path("-") : opts.output,
                                        *text, opts, invocation, diag);
        }
        if (opts.stage == Stage::preprocess) {
            std::string_view text = prep ?
                (input.language == Language::preprocessed && !preprocessor.line_markers ? prep->text : prep->output) :
                *source;
            constexpr std::size_t limit = 128u * 1024u * 1024u;
            if (text.size() > limit - preprocessed_output.size()) {
                diag.error("cephyr.limit", "combined preprocessor output exceeds 128 MiB");
                return false;
            }
            preprocessed_output += text;
            continue;
        }

        std::string assembly;
        if (c_input(input.language)) {
            const auto &source_name = prep->file;
            if (!provider && !custom_model) {
                diag.error("cephyr.target",
                           "unsupported target for C lowering: " + target->architecture + "/" + target->abi +
                           " (requires a standard target specification or an exact SDK backend for explicit LP64)",
                           Location{name});
                return false;
            }
            prep->plain_char_signed = provider && provider->plain_char_signed;
            prep->wchar_signed = !provider || provider->wchar_signed;
            prep->native_scalar_fp = native_scalar_fp;
            prep->native_rv64_soft_fp = native_rv64_lp64;
            prep->native_binary128 = native_rv64_lp64 || (native_scalar_fp && provider->architecture == "AArch64");
            prep->native_amd64_stdarg = native_scalar_fp && provider->architecture == "AMD64";
            prep->native_amd64_x87 = native_scalar_fp && provider->architecture == "AMD64";
            prep->native_aarch64_stdarg = native_scalar_fp && provider->architecture == "AArch64";
            prep->native_rv64_record_abi = native_rv64_lp64;
            if (provider && !sdk_backend)
                prep->bit_field_abi = provider->architecture == "AArch64" ?
                    Preprocessed::BitFieldABI::aapcs64 : Preprocessed::BitFieldABI::sysv_rv64;
            if (!parsed)
                parsed = measured(profile, "parse", name, [&] { return parse(*prep, diag); });
            if (!parsed || diag.error_count())
                return false;
            auto lowered = measured(profile, "lower", name, [&] {
                return lower(parsed->root(), *prep, diag, preprocessor.hosted, analysis);
            });
            if (!lowered || diag.error_count())
                return false;
            if (!measured(profile, "sdk_analyze", name, [&] {
                return sdk.analyze(lowered->mir, *target, source_name, diag);
            }))
                return false;
            if (opts.stage == Stage::syntax)
                continue;
            const bool custom_backend = opts.stage != Stage::mir && sdk_backend;
            if (qml_pipeline) {
                bool qml_changed = false;
                // This is canonical MIR from lower(); the analysis hooks
                // above cannot replace it. Verify any changed output before
                // passing it to hooks, or as usual for a custom backend.
                if (!measured(profile, "qml_rewrite", name, [&] {
                    return qml_pipeline->rewrite_canonical(lowered->mir, diag, name, &qml_changed);
                }))
                    return false;
                if ((qml_changed || custom_backend) && !measured(profile, "qml_verify", name, [&] {
                    return verify_mir(lowered->mir, diag, name, "cephyr.qml.mir");
                }))
                    return false;
            }
            bool rewritten = false;
            if (!measured(profile, "sdk_rewrite", name, [&] {
                return sdk.rewrite(lowered->mir, *target, source_name, diag, &rewritten);
            }))
                return false;
            if ((rewritten || (custom_backend && !qml_pipeline)) &&
                !verify_mir(lowered->mir, diag, name, "cephyr.sdk.mir"))
                return false;
            if (config.blackstone_enabled) {
                bool changed = false;
                if (!measured(profile, "blackstone", name, [&] {
                    return blackstone_opt::rewrite(lowered->mir, diag, name, changed);
                }))
                    return false;
            }
            std::optional<factir::Result> facts;
            if (config.factir_enabled && !custom_backend) {
                facts = measured(profile, "factir", name, [&] {
                    return factir::run(lowered->mir, *target, name, diag);
                });
                if (!facts)
                    return false;
                if (opts.verbose && facts->budget_exceeded)
                    diag.note("cephyr.factir.budget", "Datalog budget exceeded; no scheduling hints", Location{name});
            }
            const auto retain_factir_artifacts = [&] {
                if (!opts.factir_smtlib.empty() || !opts.factir_facts.empty())
                    exported_facts = std::move(facts);
            };
            if (opts.stage == Stage::mir) {
                if (!publish_text(output_name(input, opts), lowered->mir, opts, invocation, diag))
                    return false;
                retain_factir_artifacts();
                continue;
            }
            const bool pic = opts.pic.value_or(opts.shared || (opts.stage == Stage::link && !config.static_link));
            std::optional<std::string> result;
            if (custom_backend) {
                result = measured(profile, "sdk_backend", name, [&] {
                    return sdk.emit_assembly(lowered->mir, *target, source_name, pic, diag);
                });
            } else {
                result = measured(profile, "native_backend", name, [&] {
                    if (!backend)
                        backend.emplace(config, *target, diag);
                    return backend->compile(lowered->mir, name, pic, facts ? &*facts : nullptr);
                });
            }
            if (!result || diag.error_count())
                return false;
            retain_factir_artifacts();
            assembly = std::move(*result);
            if (config.sandslab_enabled && provider && !sdk_backend && provider->architecture == "AMD64")
                measured(profile, "sandslab", name, [&] { shorten_amd64_constants(assembly); });
            // Native MIR v1 does not carry C linkage. Change the single
            // binding directive at each definition, rather than emitting a
            // second conflicting directive (rejected by LLVM's assembler).
            for (const auto &symbol : lowered->local_symbols) {
                const auto global = "\t.globl " + symbol + "\n";
                const auto offset = assembly.find(global);
                if (offset == std::string::npos ||
                    assembly.find(global, offset + global.size()) != std::string::npos) {
                    diag.error("cephyr.backend.linkage", "expected one binding directive for '" + symbol + "'",
                               Location{name});
                    return false;
                }
                assembly.replace(offset, global.size(), "\t.local " + symbol + "\n");
            }
            assembly += lowered->data_assembly;
            lowered->apply_elf_names(assembly);
        } else {
            assembly = prep ? std::move(prep->text) : std::move(*source);
        }

        if (opts.stage == Stage::assembly) {
            if (!publish_text(output_name(input, opts), assembly, opts, invocation, diag))
                return false;
            continue;
        }

        const auto stem = "input-" + std::to_string(ordinal++);
        const bool anonymous = invocation.embedding() && anonymous_assembly(config, *target, opts);
        const ToolArtifact assembly_file(anonymous, (stem + ".s").c_str(), temporary);
        ToolArtifact object(anonymous, (stem + ".o").c_str(), temporary);
        assembly_file.write(assembly);
        std::vector<int> assemble_fds;
        assembly_file.inherit(assemble_fds);
        object.inherit(assemble_fds);
        if (!measured(profile, "assemble", name, [&] {
            return assemble(config, *target, opts, assembly_file.path, object.path, diag, assemble_fds);
        }))
            return false;
        if (opts.stage == Stage::object) {
            const auto output = output_name(input, opts);
            if (!publish_native(output, object.path, opts, invocation, diag,
                                object.memory ? &*object.memory : nullptr))
                return false;
        } else {
            link_inputs.push_back(object.path.string());
            if (object.memory) {
                object.inherit(link_fds);
                link_files.push_back(std::move(*object.memory));
            }
        }
    }
    if (diag.error_count())
        return false;
    if (opts.stage == Stage::preprocess)
        return publish_text(opts.output.empty() ? fs::path("-") : opts.output,
                            preprocessed_output, opts, invocation, diag);
    if (opts.stage == Stage::link && !measured(profile, "link", {}, [&] {
            return link(config, *target, opts, std::move(link_inputs), std::move(link_fds),
                        temporary, invocation, diag);
        }))
        return false;
    if (exported_facts)
        return publish_factir_artifact(opts.factir_smtlib, exported_facts->smtlib, diag) &&
               publish_factir_artifact(opts.factir_facts, exported_facts->facts, diag);
    return true;
}

// cephyr_request originally ended at sdk. Only read the appended pointer
// when present; this also accepts older C++ and ctypes clients as-is.
constexpr auto request_v1_size = offsetof(cephyr_request, lua);

const cephyr_lua_options *lua_options(const cephyr_request &request)
{
    return request.struct_size >= sizeof(cephyr_request) ? request.lua : nullptr;
}

bool valid_lua_label(std::string_view name)
{
    return !name.empty() && name.size() <= CEPHYR_LUA_MAX_NAME_BYTES &&
        std::ranges::none_of(name, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::optional<std::string> read_lua_extension(const std::string &path, Diagnostics &diag)
{
    if (!valid_lua_label(path) || path == "-") {
        diag.error("cephyr.lua.options", path == "-" ? "Lua extensions cannot be read from standard input"
                                                     : "invalid Lua extension file path");
        return std::nullopt;
    }

    struct InputFile {
        int fd;
        ~InputFile() { if (fd >= 0) close(fd); }
    } file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    const auto io_error = [&] {
        const auto error = std::error_code(errno, std::generic_category());
        diag.error("cephyr.lua.file", "cannot read Lua extension: " + error.message(), Location{path});
    };
    struct stat info{};
    if (file.fd < 0 || fstat(file.fd, &info) < 0) {
        io_error();
        return std::nullopt;
    }
    if (!S_ISREG(info.st_mode)) {
        diag.error("cephyr.lua.file", "Lua extension must be a regular file", Location{path});
        return std::nullopt;
    }
    if (info.st_size > CEPHYR_LUA_MAX_SCRIPT_BYTES) {
        diag.error("cephyr.lua.file", "Lua extension exceeds the source size limit", Location{path});
        return std::nullopt;
    }

    std::string source;
    std::array<char, 4096> buffer;
    while (true) {
        const auto count = read(file.fd, buffer.data(), buffer.size());
        if (count == 0)
            return source;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            io_error();
            return std::nullopt;
        }
        if (static_cast<std::size_t>(count) > CEPHYR_LUA_MAX_SCRIPT_BYTES - source.size()) {
            diag.error("cephyr.lua.file", "Lua extension exceeds the source size limit", Location{path});
            return std::nullopt;
        }
        source.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

bool load_lua_extensions(LuaExtensions &extensions, const cephyr_request &request,
                         const Options &opts, const ExtensionConfig &config,
                         const cephyr_sdk *native_sdk, Diagnostics &diag)
{
    const auto *lua = lua_options(request);
    const bool files_or_env = !opts.lua_files.empty() || !opts.lua_environment.empty();
    const auto invalid = [&](std::string_view reason) {
        diag.error("cephyr.lua.options", reason);
        return false;
    };
    if (lua && files_or_env)
        return invalid("request Lua options cannot be combined with driver Lua options");
    if (lua && !config.lua.empty())
        return invalid("request Lua options cannot be combined with configured Lua extensions");
    if (!lua && opts.lua_files.empty() && !opts.lua_environment.empty())
        return invalid("--lua-env requires --lua-extension");
    if (lua && !lua->script_count && lua->environment_count)
        return invalid("Lua environment entries require at least one script");
    if (config.lua.size() + opts.lua_files.size() > CEPHYR_LUA_MAX_SCRIPTS)
        return invalid("too many Lua extensions across configuration and command line");

    std::vector<std::pair<std::string, std::string>> cli_environment;
    std::vector<cephyr_lua_env_entry> cli_entries;
    for (const auto &entry : opts.lua_environment) {
        const auto pos = entry.find('=');
        if (pos == entry.npos)
            return invalid("--lua-env requires NAME=VALUE");
        cli_environment.emplace_back(entry.substr(0, pos), entry.substr(pos + 1));
    }
    for (const auto &[key, value] : cli_environment)
        cli_entries.push_back({key.c_str(), value.c_str()});
    const auto environment = lua
        ? std::span<const cephyr_lua_env_entry>(lua->environment, lua->environment_count)
        : std::span<const cephyr_lua_env_entry>(cli_entries);
    {
        const auto letter = [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
        };
        std::set<std::string_view> keys;
        for (const auto &entry : environment) {
            if (!entry.name || !entry.value)
                return invalid("Lua environment entry is missing a name or value");
            const std::string_view key(entry.name, strnlen(entry.name, CEPHYR_LUA_MAX_ENV_NAME_BYTES + 1));
            if (key.empty() || key.size() > CEPHYR_LUA_MAX_ENV_NAME_BYTES || !letter(key.front()) ||
                !std::ranges::all_of(key, [&](char ch) { return letter(ch) || (ch >= '0' && ch <= '9'); }))
                return invalid("invalid Lua environment entry name");
            if (strnlen(entry.value, CEPHYR_LUA_MAX_ENV_VALUE_BYTES + 1) > CEPHYR_LUA_MAX_ENV_VALUE_BYTES)
                return invalid("Lua environment value exceeds the size limit");
            if (!keys.insert(key).second)
                return invalid("duplicate Lua environment entry");
        }
    }

    if (!extensions.set_native(native_sdk, diag))
        return false;
    if (lua) {
        for (const auto &script : std::span(lua->scripts, lua->script_count)) {
            const std::string_view name = script.name
                ? std::string_view(script.name, strnlen(script.name, CEPHYR_LUA_MAX_NAME_BYTES + 1))
                : std::string_view("<lua-extension>");
            if (!valid_lua_label(name))
                return invalid("invalid Lua extension name");
            if (!extensions.add({script.source ? script.source : "", script.size}, name, environment, diag))
                return false;
        }
    } else {
        // Explicit CLI values were granted to CLI scripts, not to scripts
        // discovered via the driver's (possibly implicit) configuration.
        std::vector<cephyr_lua_env_entry> config_environment;
        for (const auto &[key, value] : config.lua_env)
            config_environment.push_back({key.c_str(), value.c_str()});
        for (const auto &file : config.lua) {
            const auto &path = file.native();
            const auto source = read_lua_extension(path, diag);
            if (!source || !extensions.add(*source, path, config_environment, diag))
                return false;
        }
        for (const auto &path : opts.lua_files) {
            const auto source = read_lua_extension(path, diag);
            if (!source || !extensions.add(*source, path, environment, diag))
                return false;
        }
    }
    return true;
}

int drive(Invocation &invocation)
{
    Options opts;
    Diagnostics diag;
    int status = CEPHYR_COMPILE_ERROR;
    try {
        const auto &request = invocation.request;
        SdkPipeline sdk(request.sdk);
        if (!invocation.ast_format && invocation.units.empty() && request.argc &&
            moonstone_tools::is_tool(request.argv[0])) {
            if (request.sdk || lua_options(request)) {
                diag.error("cephyr.moonstone.tool", "standalone Moonstone tools do not run SDK hooks or Lua extensions");
                status = CEPHYR_INVALID_ARGUMENT;
            } else {
                auto output = moonstone_tools::run_tool(request.argv[0],
                    {request.argv + 1, request.argc - 1}, diag);
                if (output && !diag.error_count() && publish_stdout(*output, invocation, diag))
                    status = CEPHYR_OK;
            }
        } else if (const bool parsed = parse_options(request.argc, request.argv, invocation.embedding(),
                                                      opts, diag, invocation.units, invocation.ast_format);
                   !sdk.validate(diag)) {
            status = CEPHYR_INVALID_ARGUMENT;
        } else if (parsed) {
            if (opts.help)
                publish_stdout(help_text, invocation, diag);
            if (opts.version || (opts.verbose && opts.inputs.empty()))
                publish_stdout("Cephyr " CEPHYR_VERSION "\n", invocation, diag);
            const auto compile_inputs = [&] {
                if (!opts.profile_output.empty()) {
                    if (!can_publish_profile(opts, diag))
                        return false;
                    MoonstoneProfile profile;
                    const bool ok = measured(&profile, "compile", {}, [&] {
                        return compile(opts, invocation, sdk, diag, &profile);
                    });
                    if (!publish_text(opts.profile_output, profile.report(ok && !diag.error_count()),
                                      opts, invocation, diag))
                        return false;
                    return ok;
                }
                return compile(opts, invocation, sdk, diag, nullptr);
            };
            if (opts.help || opts.version || opts.inputs.empty() || compile_inputs())
                status = diag.error_count() ? CEPHYR_COMPILE_ERROR : CEPHYR_OK;
        }
    } catch (const fs::filesystem_error &err) {
        diag.error("cephyr.driver", err.what());
    } catch (const std::exception &err) {
        diag.error("cephyr.driver", err.what());
        status = CEPHYR_INTERNAL_ERROR;
    }
    if (status != CEPHYR_OK && !diag.error_count())
        diag.error("cephyr.driver", "compilation failed");
    std::ostringstream diagnostics;
    diag.render(diagnostics, opts.sarif);
    invocation.result.diagnostics = std::move(diagnostics).str();
    return status;
}

// Callbacks must not enter the driver recursively on the same thread.
thread_local bool compiling = false;

struct CompilationScope {
    CompilationScope() { compiling = true; }
    ~CompilationScope() { compiling = false; }
};

} // namespace

int cephyr_cli_main(int argc, const char *const *argv, std::istream &input,
                    std::ostream &output, std::ostream &diagnostics)
{
    const cephyr_request request{sizeof(cephyr_request), argc > 0 ? static_cast<std::size_t>(argc - 1) : 0,
                                 argc > 0 ? argv + 1 : nullptr,
                                 nullptr, 0, nullptr, nullptr, nullptr, nullptr};
    cephyr_result result;
    Invocation invocation{request, result, &input, &output};
    CompilationScope scope;
    try {
        const int status = drive(invocation);
        diagnostics << result.diagnostics;
        return status == CEPHYR_OK ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception &error) {
        diagnostics << "error: " << error.what() << " [cephyr.driver]\n";
        return EXIT_FAILURE;
    }
}

} // namespace cephyr

unsigned cephyr_abi_version(void) { return 1; }

const char *cephyr_version(void) { return CEPHYR_VERSION; }

int cephyr::detail::compile_units(const cephyr_request *request,
                                std::span<const std::shared_ptr<const TranslationUnit>> units,
                                const HostSymbols *symbols, cephyr_result **result,
                                cephyr_ast_format ast_format)
{
    if (!result)
        return CEPHYR_INVALID_ARGUMENT;
    *result = nullptr;
    if (!request || (request->struct_size != cephyr::request_v1_size &&
                     request->struct_size < sizeof(cephyr_request)) ||
        (request->argc && !request->argv) || (request->stdin_size && !request->stdin_data) ||
        (request->resource_root && !*request->resource_root) ||
        (request->archon_root && !*request->archon_root) || cephyr::compiling)
        return CEPHYR_INVALID_ARGUMENT;
    for (std::size_t i = 0; i < request->argc; ++i)
        if (!request->argv[i])
            return CEPHYR_INVALID_ARGUMENT;
    if (const auto *lua = cephyr::lua_options(*request)) {
        if (lua->struct_size < sizeof(cephyr_lua_options) ||
            lua->script_count > CEPHYR_LUA_MAX_SCRIPTS || (lua->script_count && !lua->scripts) ||
            lua->environment_count > CEPHYR_LUA_MAX_ENV_ENTRIES ||
            (lua->environment_count && !lua->environment))
            return CEPHYR_INVALID_ARGUMENT;
        for (const auto &script : std::span(lua->scripts, lua->script_count))
            if (script.size > CEPHYR_LUA_MAX_SCRIPT_BYTES || (script.size && !script.source))
                return CEPHYR_INVALID_ARGUMENT;
    }

    try {
        cephyr::CompilationScope scope;
        auto storage = std::make_unique<cephyr_result>();
        cephyr::Invocation invocation{*request, *storage, nullptr, nullptr, units, symbols, ast_format};
        const int status = cephyr::drive(invocation);
        if (status != CEPHYR_OK)
            storage->loadable_output = false;
        *result = storage.release();
        return status;
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

int cephyr_compile(const cephyr_request *request, cephyr_result **result)
{
    return cephyr::detail::compile_units(request, {}, nullptr, result);
}

int cephyr_parse_c11(const cephyr_request *request, cephyr_ast_format format, cephyr_result **result)
{
    if (format != CEPHYR_AST_JSON && format != CEPHYR_AST_YAML && format != CEPHYR_AST_SEXP) {
        if (result)
            *result = nullptr;
        return CEPHYR_INVALID_ARGUMENT;
    }
    return cephyr::detail::compile_units(request, {}, nullptr, result, format);
}

const char *cephyr_result_output(const cephyr_result *result, size_t *size)
{
    if (size)
        *size = result ? result->output.size() : 0;
    return result ? result->output.c_str() : nullptr;
}

const char *cephyr_result_diagnostics(const cephyr_result *result, size_t *size)
{
    if (size)
        *size = result ? result->diagnostics.size() : 0;
    return result ? result->diagnostics.c_str() : nullptr;
}

void cephyr_result_destroy(cephyr_result *result) { delete result; }
