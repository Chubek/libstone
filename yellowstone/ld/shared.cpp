// shared.cpp - shared-object / PIE / relocatable drivers.
//
// Shared/PIE output requires the ELF toolchain backend. -r uses the native
// relocatable emitter in binarizer.cpp.
#include "internal.hpp"
#include "ElfBackend.hpp"
#include "ScriptSDK.hpp"

namespace qld::detail {

bool linkShared(LinkState& state, std::vector<uint8_t>& out,
                std::string& error) {
  return linkElf(state, out, error, ScriptSDK{});
}

bool linkRelocatable(LinkState& state, std::vector<uint8_t>& out,
                     std::string& error) {
  if (!assignAddresses(state, error)) return false;
  if (!emitRelocatable(state, out, error)) return false;
  return true;
}

}  // namespace qld::detail
