#include "driver.hpp"
#include "mir_verify.h"
#include "sdk_internal.hpp"

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
#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

struct cephyr_result {
    std::string output;
    std::string diagnostics;
};

namespace cephyr {
namespace {

namespace fs = std::filesystem;
using Command = std::vector<std::string>;

enum class Stage { link, object, assembly, preprocess, syntax, mir };
enum class Language { automatic, c, preprocessed, assembly, assembly_cpp, link, link_option };

struct Input {
    Language language;
    std::string argument;
};

struct Options {
    Stage stage = Stage::link;
    std::vector<Input> inputs;
    fs::path output, config_file, sysroot;
    std::string target, stdlib;
    Command assembler_options;
    PreprocessOptions preprocess;
    std::optional<bool> use_cache, static_link, line_markers, keep_comments, trigraphs, pic, hosted;
    bool no_headers = false, no_start = false, no_libraries = false, shared = false;
    bool verbose = false, sarif = false, werror = false, help = false, version = false;
};

struct Invocation {
    const cephyr_request &request;
    cephyr_result &result;
    std::istream *cli_input = nullptr;
    std::ostream *cli_output = nullptr;
};

constexpr std::string_view help_text = R"(Usage: cephyr [options] file...
Compile C11 for an Archon target (default: x86_64-linux-gnu).

  -E                        Preprocess only
  -fsyntax-only             Check the C frontend without invoking target tools
  --emit-mir                Write Archon MachineIR text
  -S                        Compile to assembly
  -c                        Compile/assemble to a relocatable object
  -o FILE                   Write the result to FILE (default link: a.out)
  -x LANG                   c, cpp-output, assembler, assembler-with-cpp, none
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
  --no-cache                Regenerate the Archon backend without using LMDB
  --diagnostics-format=FMT  text or sarif
  -Werror, -v               Promote warnings, or report compiler stages
  --help, --version         Show this help or the Cephyr version
)";

bool c_input(Language kind)
{
    return kind == Language::c || kind == Language::preprocessed;
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
    if (stage == Stage::syntax || stage == Stage::mir)
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

bool parse_options(std::size_t argc, const char *const *argv, Options &opts, Diagnostics &diag)
{
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
    if (opts.help || opts.version)
        return diag.error_count() == 0;
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
    if ((opts.stage == Stage::object || opts.stage == Stage::link) && opts.output == "-")
        diag.error("cephyr.option", "binary output to stdout is not supported");
    if (opts.shared && opts.static_link == true)
        diag.error("cephyr.option", "-shared and -static cannot be combined");
    if (opts.shared && opts.stage != Stage::link)
        diag.error("cephyr.option", "-shared requires the link stage; use -fPIC to compile an object");
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
        // Fall back to compile-time paths if discovery fails.
    }
    return {};
}();

std::pair<fs::path, fs::path> resources()
{
    auto cephyr = fs::path(CEPHYR_SOURCE_RESOURCES);
    auto archon = fs::path(CEPHYR_SOURCE_ARCHON);
    if (!module_directory.empty()) {
#ifdef CEPHYR_LIB_RELATIVE_RESOURCES
        auto installed_cephyr = module_directory / CEPHYR_LIB_RELATIVE_RESOURCES;
        auto installed_archon = module_directory / CEPHYR_LIB_RELATIVE_ARCHON;
        if (path_exists(installed_cephyr / "triples"))
            cephyr = installed_cephyr.lexically_normal();
        if (path_exists(installed_archon / "archon"))
            archon = installed_archon.lexically_normal();
#endif
    }
    return {std::move(cephyr), std::move(archon)};
}

std::optional<Config> configure(const Options &opts, const cephyr_request &request, Diagnostics &diag)
{
    const auto [cephyr, archon] = resources();
    Config config = default_config(cephyr, archon);
    auto config_file = opts.config_file;
    if (config_file.empty()) {
        if (const char *env = std::getenv("CEPHYR_CONFIG"); env && *env)
            config_file = env;
        else if (path_exists("Cephyr.toml"))
            config_file = "Cephyr.toml";
    }
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
                                          bool custom_model, bool assembler)
{
    auto result = config.preprocess;
    result.assembler = assembler;
    std::vector<std::pair<bool, std::string>> builtins{
        {true, "__CEPHYR__=1"}, {true, "__CEPHYR_HAS_VARIADIC_CALLS=1"},
        {true, "__LP64__=1"}, {true, "_LP64=1"},
    };
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
    constexpr std::size_t max_input = 16u * 1024u * 1024u;
    const std::string name = input.argument == "-" ? "<stdin>" : input.argument;
    if (input.argument == "-" && !invocation.cli_input) {
        const auto &request = invocation.request;
        if (request.stdin_size > max_input) {
            diag.error("cephyr.limit", "input exceeds the 16 MiB source limit", Location{name});
            return std::nullopt;
        }
        const std::string_view source(request.stdin_data ? request.stdin_data : "", request.stdin_size);
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
            diag.error("cephyr.limit", "input exceeds the 16 MiB source limit", Location{name});
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

class TempDirectory {
public:
    TempDirectory(const fs::path &parent, const char *pattern)
    {
        auto name = (parent / pattern).string();
        if (!mkdtemp(name.data()))
            throw fs::filesystem_error("cannot create temporary directory", parent,
                                       std::error_code(errno, std::generic_category()));
        path = std::move(name);
    }
    ~TempDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored); // only the private directory that mkdtemp created
    }
    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;
    fs::path path;
};

void save(const fs::path &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream)
        throw fs::filesystem_error("cannot write output", path,
                                   std::error_code(errno ? errno : EIO, std::generic_category()));
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

void sysroot(Command &args, const Config &config)
{
    if (!config.sysroot.empty())
        args.push_back("--sysroot=" + config.sysroot.string());
}

bool execute(const Command &args, const Options &opts, Diagnostics &diag)
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
    return run_process(args, diag) == 0 && diag.error_count() == before;
}

bool assemble(const Config &config, const Target &target, const Options &opts,
              const fs::path &input, const fs::path &output, Diagnostics &diag)
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
    return execute(cmd, opts, diag);
}

struct Runtime {
    fs::path startup, library;
};

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

const Command &runtime_compiler(const Config &config, const Target &target)
{
    if (!config.runtime_cc.empty())
        return config.runtime_cc;
    if (config.assembler_style != "driver") {
        static const Command unavailable;
        return unavailable;
    }
    return config.assembler.empty() ? target.assembler : config.assembler;
}

std::optional<Runtime> build_runtime(const Options &opts, const Config &config, const Target &target,
                                     const fs::path &workspace, Diagnostics &diag)
{
    Runtime runtime;
    if (opts.no_start && opts.no_libraries)
        return runtime;

    const auto &root = config.stdlib_root;
    if (root.empty() || !path_exists(root / "include") || !path_exists(root / "src")) {
        diag.error("cephyr.stdlib", "bundled C library headers or sources are missing", Location{root.string()});
        return std::nullopt;
    }
    Command compiler = runtime_compiler(config, target);
    if (compiler.empty()) {
        diag.error("cephyr.stdlib", "a raw assembler requires [tools].runtime_cc to build Cephyr libc");
        return std::nullopt;
    }
    if (config.runtime_cc.empty()) {
        if (config.assembler.empty())
            extend(compiler, target.assemble_flags);
        extend(compiler, config.assemble_flags);
    }
    extend(compiler, config.runtime_flags);
    sysroot(compiler, config);
    extend(compiler, {"-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                     "-fno-pic", "-fno-pie", "-nostdinc", "-I", (root / "include").string(), "-O2"});
    const auto compile = [&](std::string_view source, const fs::path &output) {
        auto command = compiler;
        const char *lang = source.ends_with(".c") ? "c" : "assembler-with-cpp";
        if (source.ends_with(".c"))
            command.push_back("-std=c11");
        extend(command, {"-c", "-x", lang, (root / "src" / source).string(), "-o", output.string()});
        return execute(command, opts, diag);
    };
    const bool amd64 = target.architecture == "AMD64";
    const bool aarch64 = target.architecture == "AArch64";
    if (!opts.no_start) {
        runtime.startup = workspace / "cephyr-crt0.o";
        if (!compile(amd64 ? "start-amd64.S" : aarch64 ? "start-aarch64.S" : "start.S", runtime.startup))
            return std::nullopt;
    }
    if (!opts.no_libraries) {
        auto command = config.archiver.empty() ? target.archiver : config.archiver;
        runtime.library = workspace / "libcephyr-c.a";
        extend(command, {"rcs", runtime.library.string()});
        for (const char *source : {amd64 ? "syscall-amd64.S" : aarch64 ? "syscall-aarch64.S" : "syscall.S",
                                   "runtime.c", "allocator.c", "assert.c",
                                   "ctype.c", "stdlib.c", "stdio.c", "stream.c", "string.c"}) {
            const auto object = workspace / ("libc-" + fs::path(source).stem().string() + ".o");
            if (!compile(source, object))
                return std::nullopt;
            command.push_back(object.string());
        }
        if (!execute(command, opts, diag))
            return std::nullopt;
    }
    return runtime;
}

bool link(const Config &config, const Target &target, const Options &opts,
          Command inputs, const fs::path &workspace, Diagnostics &diag)
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
    if (!opts.shared)
        cmd.push_back(driver ? "-Wl,--fatal-warnings" : "--fatal-warnings");

    Runtime runtime;
    if (config.stdlib == "cephyr" && !(opts.no_start && opts.no_libraries)) {
        auto built = build_runtime(opts, config, target, workspace, diag);
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
    if (!runtime.library.empty())
        cmd.push_back(runtime.library.string());
    const auto temporary = workspace / "link-result";
    extend(cmd, {"-o", temporary.string()});
    return execute(cmd, opts, diag) && publish_file(output, temporary, opts, diag);
}

bool check_auto_tools(const Options &opts, const Config &config, const Target &target,
                      bool sdk_backend, Diagnostics &diag)
{
    if (opts.stage == Stage::preprocess || opts.stage == Stage::syntax || opts.stage == Stage::mir)
        return true;
    const auto check = [&](std::string_view role, const Command &command) {
        return check_auto_tool(target, role, command, diag);
    };
    const bool has_c = std::any_of(opts.inputs.begin(), opts.inputs.end(),
                                  [](const Input &input) { return c_input(input.language); });
    const bool has_source = std::any_of(opts.inputs.begin(), opts.inputs.end(),
                                  [](const Input &input) { return source_input(input.language); });
    if (has_c && !sdk_backend &&
        !check("a host C compiler to build the Archon backend", config.host_cc))
        return false;
    if (has_source && (opts.stage == Stage::object || opts.stage == Stage::link) &&
        !check("a target assembler", config.assembler.empty() ? target.assembler : config.assembler))
        return false;
    if (opts.stage != Stage::link)
        return true;
    if (!check("a target linker", config.linker.empty() ? target.linker : config.linker))
        return false;
    if (config.stdlib != "cephyr" || (opts.no_start && opts.no_libraries))
        return true;
    return check("a target compiler ([tools].runtime_cc or the assembler driver) to build Cephyr libc",
                 runtime_compiler(config, target)) &&
           (opts.no_libraries ||
            check("a target archiver", config.archiver.empty() ? target.archiver : config.archiver));
}

bool compile(const Options &opts, Invocation &invocation, SdkPipeline &sdk, Diagnostics &diag)
{
    auto loaded = configure(opts, invocation.request, diag);
    if (!loaded)
        return false;
    auto &config = *loaded;
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
    const auto *provider = native_lowering_provider(target->architecture, target->abi);
    if (provider) {
        std::error_code error;
        if (!fs::equivalent(target->specification, config.archon_root / provider->stdspec, error))
            provider = nullptr;
    }
    const bool sdk_backend = sdk.has_backend(*target);
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
    if (automatic && !check_auto_tools(opts, config, *target, sdk_backend, diag))
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

    std::optional<TempDirectory> workspace;
    const auto temporary = [&]() -> const fs::path & {
        if (!workspace)
            workspace.emplace(fs::absolute(fs::temp_directory_path()), "cephyr-XXXXXX");
        return workspace->path;
    };
    std::optional<Backend> backend;
    const auto preprocessor = preprocessor_options(opts, config, *target, provider, custom_model, false);
    const auto assembler_preprocessor = preprocessor_options(opts, config, *target, provider, custom_model, true);
    Command link_inputs;
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
        auto source = read_input(input, invocation, diag);
        if (!source)
            return false;
        const auto name = input.argument == "-" ? "<stdin>" : input.argument;
        if (input.language == Language::c && !sdk.desugar(*source, *target, name, diag))
            return false;
        std::optional<Preprocessed> prep;
        if (input.language == Language::preprocessed) {
            prep = read_preprocessed(*source, name, diag);
        } else if (input.language == Language::c || input.language == Language::assembly_cpp) {
            prep = preprocess(*source, name, input.language == Language::c ? preprocessor : assembler_preprocessor, diag);
        }
        const bool expects_preprocessor = input.language != Language::assembly;
        if ((expects_preprocessor && !prep) || diag.error_count())
            return false;
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
            if (!provider && !custom_model) {
                diag.error("cephyr.target",
                           "unsupported target for C lowering: " + target->architecture + "/" + target->abi +
                           " (requires a standard target specification or an exact SDK backend for explicit LP64)",
                           Location{name});
                return false;
            }
            prep->plain_char_signed = provider && provider->plain_char_signed;
            auto parsed = parse(*prep, diag);
            if (!parsed || diag.error_count())
                return false;
            auto lowered = lower(parsed->root(), *prep, diag, preprocessor.hosted);
            if (!lowered || diag.error_count())
                return false;
            if (!sdk.analyze(lowered->mir, *target, name, diag))
                return false;
            if (opts.stage == Stage::syntax)
                continue;
            bool rewritten = false;
            if (!sdk.rewrite(lowered->mir, *target, name, diag, &rewritten))
                return false;
            const bool custom_backend = opts.stage != Stage::mir && sdk_backend;
            if (rewritten || custom_backend) {
                ArchonBackendDiagnostic verification{};
                if (cephyr_verify_mir(lowered->mir.data(), lowered->mir.size(), &verification) !=
                    ARCHON_BACKEND_OK) {
                    diag.error("cephyr.sdk.mir", verification.message[0] ? verification.message :
                               "Archon rejected MachineIR", Location{name});
                    return false;
                }
            }
            if (opts.stage == Stage::mir) {
                if (!publish_text(output_name(input, opts), lowered->mir, opts, invocation, diag))
                    return false;
                continue;
            }
            const bool pic = opts.pic.value_or(opts.shared || (opts.stage == Stage::link && !config.static_link));
            std::optional<std::string> result;
            if (custom_backend) {
                result = sdk.emit_assembly(lowered->mir, *target, name, pic, diag);
            } else {
                if (!backend)
                    backend.emplace(config, *target, temporary() / "backend", diag);
                result = backend->compile(lowered->mir, name, pic);
            }
            if (!result || diag.error_count())
                return false;
            assembly = std::move(*result);
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
        } else {
            assembly = prep ? std::move(prep->text) : std::move(*source);
        }

        if (opts.stage == Stage::assembly) {
            if (!publish_text(output_name(input, opts), assembly, opts, invocation, diag))
                return false;
            continue;
        }

        const auto stem = "input-" + std::to_string(ordinal++);
        const auto assembly_file = temporary() / (stem + ".s");
        const auto object = temporary() / (stem + ".o");
        save(assembly_file, assembly);
        if (!assemble(config, *target, opts, assembly_file, object, diag))
            return false;
        if (opts.stage == Stage::object) {
            const auto output = output_name(input, opts);
            if (!publish_file(output, object, opts, diag))
                return false;
        } else {
            link_inputs.push_back(object.string());
        }
    }
    if (diag.error_count())
        return false;
    if (opts.stage == Stage::preprocess)
        return publish_text(opts.output.empty() ? fs::path("-") : opts.output,
                            preprocessed_output, opts, invocation, diag);
    if (opts.stage == Stage::link)
        return link(config, *target, opts, std::move(link_inputs), temporary(), diag);
    return true;
}

int drive(Invocation &invocation)
{
    Options opts;
    Diagnostics diag;
    int status = CEPHYR_COMPILE_ERROR;
    try {
        const auto &request = invocation.request;
        const bool parsed = parse_options(request.argc, request.argv, opts, diag);
        SdkPipeline sdk(request.sdk);
        if (!sdk.validate(diag)) {
            status = CEPHYR_INVALID_ARGUMENT;
        } else if (parsed) {
            if (opts.help)
                publish_stdout(help_text, invocation, diag);
            if (opts.version || (opts.verbose && opts.inputs.empty()))
                publish_stdout("Cephyr " CEPHYR_VERSION "\n", invocation, diag);
            if (opts.help || opts.version || opts.inputs.empty() || compile(opts, invocation, sdk, diag))
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
                                 nullptr, 0, nullptr, nullptr, nullptr};
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

int cephyr_compile(const cephyr_request *request, cephyr_result **result)
{
    if (!result)
        return CEPHYR_INVALID_ARGUMENT;
    *result = nullptr;
    if (!request || request->struct_size < sizeof(cephyr_request) ||
        (request->argc && !request->argv) || (request->stdin_size && !request->stdin_data) ||
        (request->resource_root && !*request->resource_root) ||
        (request->archon_root && !*request->archon_root) || cephyr::compiling)
        return CEPHYR_INVALID_ARGUMENT;
    for (std::size_t i = 0; i < request->argc; ++i)
        if (!request->argv[i])
            return CEPHYR_INVALID_ARGUMENT;

    try {
        cephyr::CompilationScope scope;
        auto storage = std::make_unique<cephyr_result>();
        cephyr::Invocation invocation{*request, *storage};
        const int status = cephyr::drive(invocation);
        *result = storage.release();
        return status;
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
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
