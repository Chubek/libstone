#pragma once

#include "WASM-Module.hpp"

namespace limestone::wasmutils::core {

struct ObjectLinkInput {
  std::string_view name; // Used for diagnostics.
  std::span<const std::uint8_t> bytes;
};

struct ObjectLinkOptions {
  // Additional object symbol names to export, including hidden definitions.
  // A data symbol is exported as an immutable i32 address global.
  std::span<const std::string_view> exports;
};

// Statically link tool-conventions v2 wasm32 objects into one Core 2.0 module.
// Use the original bytes: relocation offsets refer to the input binary and
// cannot be recovered from a decoded/re-encoded Module. See WASM-Object-Link.md
// for the supported ABI, relocations, and intentionally unsupported features.
Module link_objects(std::span<const ObjectLinkInput> inputs,
                    const ObjectLinkOptions& options = {});

} // namespace limestone::wasmutils::core
