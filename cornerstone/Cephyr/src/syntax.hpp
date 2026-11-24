#pragma once

#include <cephyr/Cephyr.h>
#include <cephyr/parser.hpp>
#include <cephyr/preprocess.hpp>

#include <cstddef>

namespace cephyr::syntax {

inline constexpr std::size_t max_document_bytes = 64u * 1024u * 1024u;

struct Unit {
    Preprocessed source;
    ParsedUnit parsed;
};

std::optional<std::string> emit(cephyr_ast_format, const ParsedUnit &, const Preprocessed &,
                                Diagnostics &);
// Validate the serialized tree and rebuild the frontend's parser-owned bindings.
std::optional<Unit> read(cephyr_ast_format, std::string_view, const std::string &name,
                         Diagnostics &);

} // namespace cephyr::syntax
