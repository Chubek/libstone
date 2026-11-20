// static.cpp - static executable link driver.
//
// Orchestrates layout, relocation and emission for static ET_EXEC.
#include "internal.hpp"

namespace qld::detail {

bool linkStatic(LinkState& state, std::vector<uint8_t>& out,
                std::string& error) {
  const uint64_t addressLimit = state.wide ? UINT64_MAX : UINT32_MAX;
  if (state.ltoPlan.gcSections && state.options.hasBase &&
      state.options.base > addressLimit - ltoImageLimit) {
    error = "--lto-script section GC: image base too close to address-space limit";
    return false;
  }
  if (!assignAddresses(state, error)) return false;
  if (state.ltoPlan.gcSections) {
    if (state.outSections.size() >= ltoSectionLimit) {
      error = "--lto-script section GC: output section count limit exceeded";
      return false;
    }
    for (const auto& section : state.outSections) {
      if (section.size > ltoImageLimit || section.fileOffset > ltoImageLimit ||
          section.fileSize > ltoImageLimit - section.fileOffset ||
          section.vma > addressLimit || section.size > addressLimit - section.vma) {
        error = "--lto-script section GC: output section exceeds image limits: " + section.name;
        return false;
      }
    }
  }
  // Stage section bytes then relocate in place.
  std::vector<std::vector<uint8_t>> staged(state.outSections.size());
  for (size_t o = 0; o < state.outSections.size(); ++o) {
    const auto& sec = state.outSections[o];
    if (sec.bss) continue;
    std::vector<uint8_t> buf = sec.scriptData;
    buf.resize(size_t(sec.size), 0);
    for (size_t idx : sec.inputs) {
      const auto& in = state.inputSections[idx];
      size_t n = in.contents.size();
      size_t cap = size_t(sec.size) > in.outOffset
                       ? size_t(sec.size - in.outOffset)
                       : 0;
      if (n > cap) n = cap;
      if (n) std::copy(in.contents.begin(), in.contents.begin() + n,
                       buf.begin() + ptrdiff_t(in.outOffset));
    }
    staged[o] = std::move(buf);
  }
  if (!applyRelocations(state, staged, error)) return false;
  if (!emitExecutable(state, staged, out, error)) return false;
  return true;
}

}  // namespace qld::detail
