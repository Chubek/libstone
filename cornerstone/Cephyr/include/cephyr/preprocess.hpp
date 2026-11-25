#pragma once

#include <cephyr/diagnostics.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
    bool native_atomics = false;
    // Allow hash-prefixed assembler comments in addition to CPP directives.
    bool assembler = false;
    // Target properties for C character constants in #if and #elif.
    bool plain_char_signed = false;
    bool wchar_signed = true;
    // Only registered names for the current target are recognized under GCC.
    std::vector<std::string> gcc_pragma_names;
    // Dispatch for active SDK pragmas in raw C, after ucpp releases its lock.
    std::function<bool(std::string_view name, std::string_view arguments,
                       const Location &, std::optional<std::string> &replacement)> directive;
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
    // Serialized inputs use sparse runs instead of expanding file names.
    std::vector<std::pair<unsigned, Location>> line_runs;
    std::string file;
    // Target property for semantic analysis, independent of user CPP macros.
    bool plain_char_signed = false;
    // Set by the driver only for reviewed hardware floating-point ABI routes.
    bool native_scalar_fp = false;
    bool native_vla = false; // native provider supports dynamic stack frames
    bool native_atomics = false;
    // RV64IM LP64 binary32/binary64, with runtime calls for arithmetic and casts.
    bool native_rv64_soft_fp = false;
    bool native_fp_types() const { return native_scalar_fp || native_rv64_soft_fp; }
    bool native_amd64_stdarg = false;
    // The bundled AMD64 SysV frontend knows the x87 extended object format.
    bool native_amd64_x87 = false;
    bool native_binary128 = false;
    bool native_aarch64_stdarg = false;
    // RV64IM's soft-float ABI also defines the native variadic cursor.
    bool native_scalar_stdarg() const {
        return native_amd64_stdarg || native_aarch64_stdarg || native_rv64_soft_fp;
    }
    // Signedness of the target's 32-bit wchar_t, not a preprocessor macro.
    bool wchar_signed = true;
    // Only the bundled RV64IM LP64 backend has a reviewed record ABI here.
    bool native_rv64_record_abi = false;
    // Bundled little-endian ABIs agree on allocation order but differ in
    // whether an unnamed bit-field contributes to the enclosing alignment.
    enum class BitFieldABI { unavailable, sysv_rv64, aapcs64 };
    BitFieldABI bit_field_abi = BitFieldABI::unavailable;

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
