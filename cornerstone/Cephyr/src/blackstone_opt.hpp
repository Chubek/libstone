#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cephyr {
class Diagnostics;

namespace blackstone_opt {
// Optimize a verified Archon MachineIR v1 module transactionally. Returns
// false with a diagnostic on failure. changed reports a committed byte edit.
bool rewrite(std::string &mir, Diagnostics &diag, std::string_view input, bool &changed,
             std::uint64_t passes = ~std::uint64_t{0});
}
}
