#pragma once
#include "CDecl.hpp"

namespace moonstone::exoml::cdecl {
struct ExtractLimits {
    std::size_t declarations = 16384, types = 16384, depth = 128;
    std::size_t input_bytes = 64 * 1024 * 1024, entries = 131072;
    std::size_t cache_bytes = 128 * 1024 * 1024;
};
struct ExtractOptions {
    std::string header, revision, target, resource_directory;
    std::vector<std::string> include_directories, defines, undefines;
    // Additional provenance inputs, e.g. MANIFEST.yaml or a future sidecar.
    std::vector<std::string> provenance_files;
    ExtractLimits limits;
    // Opt-in compiler-validated normalization cache. Linux, private directory.
    // Empty disables caching; the compiler still parses before every cache hit.
    std::string cache_directory;
    // Optional compile_commands.json and exact source-entry selector. No command executes.
    std::string compile_commands, compile_file;
    // Restricted, tokenized compiler options (never a shell command); appended after the DB.
    std::vector<std::string> compiler_arguments;
};
enum class CacheStatus { Disabled, Miss, Hit, Rebuilt, Bypassed, Error };
struct CacheReport {
    CacheStatus status = CacheStatus::Disabled;
    std::string key, reason;
    bool stored = false;
};
// Link Moonstone::ExoMLExtract (opt-in), not just Moonstone::ExoML.
// Synchronous; owns its Clang context, no global mutable state. Never invokes
// native functions. Returns partial evidence plus errors on source/limit failure.
TranslationUnit extract_c11(const ExtractOptions &options);
// Same extraction; reports cache activity separately from deterministic metadata.
TranslationUnit extract_c11(const ExtractOptions &options, CacheReport &cache);
} // namespace moonstone::exoml::cdecl
