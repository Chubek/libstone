// shared.cpp - shared-object / PIE / relocatable drivers.
//
// ELF shared objects, PIEs and dynamically linked executables use the native
// dynamic emitter. -r uses the native relocatable emitter in binarizer.cpp.
#include "internal.hpp"

namespace qld::detail {

bool linkShared(LinkState& state, std::vector<uint8_t>& out,
                std::string& error) {
  if (hasScript(state)) {
    error = "native dynamic ELF linker scripts are not yet supported";
    return false;
  }
  if (!assignAddresses(state, error)) return false;
  return emitDynamicElf(state, out, error);
}

bool linkRelocatable(LinkState& state, std::vector<uint8_t>& out,
                     std::string& error) {
  if (!assignAddresses(state, error)) return false;
  if (!emitRelocatable(state, out, error)) return false;
  return true;
}

}  // namespace qld::detail
