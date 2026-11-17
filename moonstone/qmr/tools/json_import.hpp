#pragma once

// Contract-validated JSON import for the qmr CLI (not part of the core
// library). The vendored klib parser (third_party/klib kson) builds the
// tree; a strict RFC 8259 grammar walk runs first so only standard JSON
// ever reaches kson, and the tree walk enforces depth/byte budgets and
// duplicate-key rejection before any value enters a template context.

#include "qmr/Qmr.hpp"

namespace moonstone::qmr::cli {

// Parses file contents as strict JSON. Contract: the document root must be
// an object; nesting is bounded by limits.max_value_depth; total scalar
// bytes by limits.max_context_bytes; duplicate keys, trailing content and
// non-standard extensions (unquoted keys, single quotes, comments, extra
// commas) are rejected. Integers that fit int64_t stay integers; all other
// numbers become doubles.
Context load_json_context(const std::filesystem::path &file, const Limits &limits);

} // namespace moonstone::qmr::cli
