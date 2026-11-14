#pragma once
#include <exoml/Extract.hpp>

namespace moonstone::exoml::cdecl::detail {
struct CompileOptions {
    std::vector<std::string> arguments;
    std::optional<CompilationContext> context;
    std::optional<InputFile> database;
};
CompileOptions compilation_options(const ExtractOptions &options);
} // namespace moonstone::exoml::cdecl::detail
