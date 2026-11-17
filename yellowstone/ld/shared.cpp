// shared.cpp - shared-object / PIE / relocatable drivers.
//
// Shared/PIE output is not implemented natively. -r uses the native
// relocatable emitter in binarizer.cpp.
#include "internal.hpp"

namespace qld::detail {

bool linkShared(LinkState& state, std::vector<uint8_t>& out,
                std::string& error) {
  (void)state; (void)out;
  error = "native shared/PIE linking is not yet supported";
  return false;
}

bool linkRelocatable(LinkState& state, std::vector<uint8_t>& out,
                     std::string& error) {
  if (!assignAddresses(state, error)) return false;
  if (!emitRelocatable(state, out, error)) return false;
  return true;
}

}  // namespace qld::detail
