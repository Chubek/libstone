#pragma once

#include <cephyr/diagnostics.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cephyr {

struct PreprocessOptions {
    std::vector<std::filesystem::path> include_dirs;
    std::vector<std::filesystem::path> system_include_dirs;
    std::vector<std::filesystem::path> forced_includes;
    std::vector<std::pair<bool, std::string>> macros;
    // C11 phase 1 replaces trigraphs before phase 2 splices source lines.
    bool trigraphs = true;
    bool keep_comments = false;
    bool line_markers = true;
    // Controls the built-in __STDC_HOSTED__ value (1 hosted, 0 freestanding).
    bool hosted = true;
    // Allow hash-prefixed assembler comments in addition to CPP directives.
    bool assembler = false;
};

struct Preprocessed {
    // Parser input, with generated line markers removed (not lookalike text
    // produced by a macro). `lines` maps each output line to presumed source.
    std::string text;

    // The ordinary preprocessor output. Unlike `text`, this retains line
    // markers when they were requested.
    std::string output;

    // One entry per one-based line in `text`.
    std::vector<Location> lines;
    std::string file;
    // Target property for semantic analysis, independent of user CPP macros.
    bool plain_char_signed = false;
    // Set by the driver only for reviewed hardware floating-point ABI routes.
    bool native_scalar_fp = false;
    // RV64IM LP64 storage, transport, sign negation and ordered comparisons.
    bool native_rv64_soft_fp = false;
    bool native_fp_types() const { return native_scalar_fp || native_rv64_soft_fp; }
    bool native_amd64_stdarg = false;
    // The bundled AMD64 SysV frontend knows the x87 extended object format.
    bool native_amd64_x87 = false;
    bool native_aarch64_stdarg = false;
    bool native_scalar_stdarg() const { return native_amd64_stdarg || native_aarch64_stdarg; }
    // Signedness of the target's 32-bit wchar_t, not a preprocessor macro.
    bool wchar_signed = true;
    // Only the bundled RV64IM LP64 backend has a reviewed record ABI here.
    bool native_rv64_record_abi = false;

    Location locate(unsigned line, unsigned column = 0) const;
};

std::optional<Preprocessed> preprocess(const std::string &source,
                                       const std::string &file,
                                       const PreprocessOptions &options,
                                       Diagnostics &diagnostics);

std::optional<Preprocessed> read_preprocessed(const std::string &source,
                                              const std::string &file,
                                              Diagnostics &diagnostics);

} // namespace cephyr
