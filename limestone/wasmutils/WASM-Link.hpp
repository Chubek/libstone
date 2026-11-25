#pragma once

#include "WASM-Module.hpp"

namespace limestone::wasmutils::core {

// Statically combine Core 2.0 modules. An import's field name binds to a
// same-named export from another input, regardless of the import namespace.
// Unmatched imports remain host imports, coalesced by namespace and field.
// Export names must be unique across inputs. Segments keep input order, and
// start functions run in input order after the combined module's segments.
// Core 2.0 allows at most one memory, so independent memories are rejected.
// Custom metadata is omitted. For relocatable objects use link_objects from
// WASM-Object-Link.hpp, which takes original binary bytes. Dynamic-linking
// sections are rejected. Invalid modules and symbols throw wasmutils::Error.
Module link_modules(std::span<const Module> modules);

struct LinkInput {
  std::string_view name;
  const Module* module;
};

// Named inputs match the import namespace exactly. If the namespace names an
// input, that input must export the requested field; otherwise the import remains
// external. Input names must be nonempty and unique. Repeated export names are
// qualified as NAME.FIELD in the flat output; remaining collisions are errors.
Module link_modules(std::span<const LinkInput> inputs);

} // namespace limestone::wasmutils::core
