#ifndef CEPHYR_CONFIG_HPP
#define CEPHYR_CONFIG_HPP

#include <cephyr/preprocess.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cephyr {

class Diagnostics;

struct QmlProfile {
    std::vector<std::string> passes;
    std::vector<std::filesystem::path> files;
    std::optional<unsigned> fuel;
    std::map<std::string, unsigned, std::less<>> phase_fuel;
};

struct ExtensionConfig {
    std::vector<std::filesystem::path> lua;
    std::vector<std::filesystem::path> native;
    std::map<std::string, std::string, std::less<>> lua_env;
};

struct Config {
    std::optional<std::string> target; // unset uses the driver default (or the explicit script)

    std::filesystem::path triple_dir;
    std::filesystem::path triple_script;
    std::filesystem::path archon_root;
    std::filesystem::path stdlib_root;
    std::filesystem::path package_root; // Derived from the loaded library, never from the cache.
    std::filesystem::path cache_path;
    std::filesystem::path sysroot;

    std::string representation = "gated";
    // "driver" invokes clang/gcc with -c -x assembler; "as" invokes a raw
    // GNU-compatible assembler with -o.
    std::string assembler_style = "driver";

    // Tools are argv vectors, never shell command lines. The first element is
    // the executable and the remaining elements are fixed arguments. Empty
    // target-tool vectors inherit the selected target-bank entry.
    std::vector<std::string> assembler;
    std::vector<std::string> linker;
    std::vector<std::string> assemble_flags;
    std::vector<std::string> link_flags;

    PreprocessOptions preprocess;
    ExtensionConfig extensions;
    std::optional<std::string> qml_profile;
    std::map<std::string, QmlProfile, std::less<>> qml_profiles;

    bool cache_enabled = true;
    bool blackstone_enabled = true;
    bool sandslab_enabled = false;
    bool factir_enabled = true;
    bool static_link = true;
    // Driver-only CLI state; intentionally not accepted from Cephyr.toml.
    bool verbose = false;
    std::string stdlib = "cephyr";
};

Config default_config(const std::filesystem::path &resources,
                      const std::filesystem::path &archon_root);

// Recognizes common raw ld-family commands. A system-libc link requires a
// compiler driver; validate once after CLI overrides, not while parsing TOML.
bool is_raw_linker_command(const std::vector<std::string> &argv);

// Applies one strict TOML file transactionally. On failure, config is
// unchanged and one or more diagnostics are emitted.
bool load_config(Config &config, const std::filesystem::path &path,
                 Diagnostics &diagnostics);

} // namespace cephyr

#endif
