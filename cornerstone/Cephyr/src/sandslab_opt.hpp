#pragma once
#include <string>

namespace cephyr {
// Input must be unmodified native AMD64GAS output (before appended data).
void shorten_amd64_constants(std::string &assembly);
}
