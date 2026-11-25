#pragma once

#include "WASM-Module.hpp"

namespace limestone::wasmutils::core {

// Print a validated Core module as standard, index-based WAT.
// Custom sections have no Core text representation and are not printed.
std::string print_module(const Module& module);

} // namespace limestone::wasmutils::core
