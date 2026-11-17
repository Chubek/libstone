// Internal toolchain bridge. No subprocess or platform types in public headers.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace qld { class ScriptSDK; }
namespace qld::detail {
struct LinkOptions;
struct LinkState;
bool needsElfBackend(const LinkOptions&);
const char *defaultElfLinker();
bool linkElf(LinkState&, std::vector<uint8_t>&, std::string&, const ScriptSDK&);
}
