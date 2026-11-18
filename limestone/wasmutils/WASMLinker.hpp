#pragma once

#include "WASM-IR.hpp"

namespace limestone::wasmutils {

// Concatenate separately decodable raw fragments in order. No instruction may
// span fragments. Errors use offsets in the prospective combined stream. There
// are no relocations or module semantics; validate stack effects separately.
std::vector<std::uint8_t> link(
    std::span<const std::span<const std::uint8_t>> fragments);

} // namespace limestone::wasmutils
