#ifndef CEPHYR_CONFIG_HPP
#define CEPHYR_CONFIG_HPP

#include <cephyr/preprocess.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cephyr {

class Diagnostics;

struct Config {
    std::optional<std::string> target; // unset uses the driver default (or the explicit script)

    std::filesystem::path triple_dir;
    std::filesystem::path triple_script;
    std::filesystem::path archon_root;
    std::filesystem::path stdlib_root;
    std::filesystem::path cache_path;
    std::filesystem::path sysroot;

    std::string representation = "gated";
    // "driver" invokes clang/gcc with -c -x assembler; "as" invokes a raw
    // GNU-compatible assembler with -o.
    std::string assembler_style = "driver";

    // Tools are argv vectors, never shell command lines. The first element is
    // the executable and the remaining elements are fixed arguments. Empty
    // target-tool vectors inherit the selected target-bank entry.
    std::vector<std::string> host_cc{"cc"};
    std::vector<std::string> assembler;
    std::vector<std::string> linker;
    std::vector<std::string> archiver;

    // Used only to build Cephyr's packaged libc. It is deliberately separate
    // from host_cc and is not a facility for compiling user C source.
    // Empty means to inherit the resolved target assembler command.
    std::vector<std::string> runtime_cc;

    std::vector<std::string> compile_flags;
    std::vector<std::string> assemble_flags;
    std::vector<std::string> link_flags;
    // Additional flags for the packaged-libc compiler. When runtime_cc is
    // absent these follow the resolved target assembler flags.
    std::vector<std::string> runtime_flags;

    PreprocessOptions preprocess;

    bool cache_enabled = true;
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
