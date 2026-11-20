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
    bool trigraphs = true;
    bool keep_comments = false;
    bool line_markers = true;
    // Controls the built-in __STDC_HOSTED__ value (1 hosted, 0 freestanding).
    bool hosted = true;
    // Allow hash-prefixed assembler comments in addition to CPP directives.
    bool assembler = false;
};

struct Preprocessed {
    // Parser input. Preprocessor line markers are removed while `lines`
    // retains the corresponding source location for every line in `text`.
    std::string text;

    // The ordinary preprocessor output. Unlike `text`, this retains line
    // markers when they were requested.
    std::string output;

    // One entry per one-based line in `text`.
    std::vector<Location> lines;
    std::string file;
    // Target property for semantic analysis, independent of user CPP macros.
    bool plain_char_signed = false;

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
