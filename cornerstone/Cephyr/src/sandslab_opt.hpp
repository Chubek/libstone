#pragma once
#include <cstdint>
#include <string>

namespace cephyr {
// Input must be unmodified native AMD64GAS output (before appended data).
void shorten_amd64_constants(std::string &assembly, std::uint64_t passes = ~std::uint64_t{0});
}
