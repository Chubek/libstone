#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace moonstone::exotract {
namespace fs = std::filesystem;
inline constexpr unsigned schema_version = 2;

struct Diagnostic {
    enum class Severity { Note, Warning, Error } severity = Severity::Error;
    std::string code, message;
};
struct Command {
    std::vector<std::string> argv; // argv tokens, never interpreted by a shell
};
struct Backend {
    std::string main = "builtin";
    std::vector<Command> fallback;
};
struct HeaderOnly {
    std::string style = "implementation-macro";
    std::string declaration_guard, implementation_guard;
    std::vector<std::string> implementation_defines;
    std::vector<std::string> dependencies;
    unsigned implementation_translation_units = 1;
};
struct Config {
    std::string name, summary, documentation, cstd = "c11";
    fs::path path;
    bool cache = false;
    HeaderOnly header_only;
    Backend preprocessor, parser;
    std::vector<std::string> defines, undefines, required_symbols;
    std::vector<fs::path> include_directories;
    std::string compiler = "cc";
    std::string exoml_bind; // path to existing `exoml-bind`, or empty when unavailable
    std::string clang_resource_dir;
    std::string target, revision;
    std::vector<std::string> compiler_arguments;
    std::vector<fs::path> provenance_files;
    fs::path cache_dir;
    std::size_t max_output_bytes = 32 * 1024 * 1024;
    unsigned command_timeout_ms = 15000;
};
struct Symbol {
    std::string name, kind, type, linkage;
    bool supported = false;
};
struct Result {
    std::string preprocessed, cdecl_json, fingerprint;
    std::vector<Symbol> symbols;
    std::vector<Diagnostic> diagnostics;
    bool preprocessed_ok = false, declarations_complete = false, syntax_checked = false;
    bool cache_hit = false;
    bool externally_imported = false;
    std::string preprocessor_used, parser_used, compiler_identity;
    bool ok() const;
};

// A bounded POSIX subprocess driver. No shell interpolation; stderr and stdout
// are drained concurrently; failures, timeouts and overlarge output fail closed.
struct CommandResult {
    int exit_code = -1;
    std::string out, err;
    bool timed_out = false, overflow = false;
    bool ok() const { return exit_code == 0 && !timed_out && !overflow; }
};
CommandResult run_command(const Command &command, const Config &config,
                          const std::map<std::string, std::string> &substitutions = {});

// LMDB-backed when EXOTRACT_WITH_LMDB is built, otherwise a bounded in-memory
// store. A cache request without LMDB deliberately fails (no false durability).
class Store {
public:
    explicit Store(fs::path directory = {});
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    std::optional<std::string> get(std::string_view key) const;
    void put(std::string_view key, std::string_view value);
    bool persistent() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class GlobalRegistry {
public:
    explicit GlobalRegistry(std::shared_ptr<Store> store);
    void set(std::string name, std::string value);
    std::optional<std::string> get(std::string_view name) const;
    std::string expand(std::string_view input) const;
private:
    std::shared_ptr<Store> store_;
};

class Engine {
public:
    explicit Engine(Config config, std::shared_ptr<Store> store = nullptr,
                    std::shared_ptr<GlobalRegistry> globals = nullptr);
    Result preprocess() const;
    Result parse(Result previous = {}) const;
    Result inspect() const;
    Result import_cdecl(Result previous, std::string json) const;
    bool cache(const Result &result) const;
    std::string manifest_json(const Result &result) const;
    std::string implementation_source() const;
    std::string on1x_manifest_json(const Result &result) const;
    // Canonical ExoML structural/semantic validation and normalization only;
    // preserves the sidecar's trust claims without authorizing safe exports.
    std::string validate_contract(const Result &result, const fs::path &sidecar) const;
    const Config &config() const { return config_; }
private:
    Config config_;
    std::shared_ptr<Store> store_;
    std::shared_ptr<GlobalRegistry> globals_;
    std::string expanded_path() const;

};

std::string json_quote(std::string_view text);
std::string read_file(const fs::path &p, std::size_t max_bytes = 64 * 1024 * 1024);
void write_file_atomic(const fs::path &path, std::string_view contents);
void validate_config(const Config &cfg);
std::string diagnostic_json(const Diagnostic &d);
} // namespace moonstone::exotract
