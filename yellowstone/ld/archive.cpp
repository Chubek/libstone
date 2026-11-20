// archive.cpp - archive container helpers for the linker.
//
// Archive member selection itself lives in discover_symbols.cpp; this file
// owns the small helpers: expanding a container into candidates and reading
// one member's bytes through qArchive.hpp.
#include "internal.hpp"

namespace qld::detail {

std::vector<ArchiveCandidate> expandArchive(LoadedArchive& archive, bool lto,
                                             std::string& error) {
  std::vector<ArchiveCandidate> out;
  // Index archiveIndex by position in LinkState::archives. The caller passes
  // the live element, so we cannot know its index here; it is filled in by
  // discover_symbols.cpp after the call.
  for (const auto& m : archive.archive->members()) {
    if (m.special) continue;
    auto payload = archive.archive->readMember(m);
    if (!payload) {
      error = archive.path + "(" + m.name +
              "): " + payload.error().message;
      return {};
    }
    ArchiveCandidate c;
    c.archiveIndex = 0;  // patched by caller
    c.member = m;
    c.bytes.assign(payload->begin(), payload->end());
    if (lto) {
      const auto label = archive.path + "(" + m.name + ")";
      // Inspect all members, including ones not selected by symbol discovery.
      // In particular, a mixed archive must not hide bitcode or dynamic inputs.
      if (!validateLtoInput(c.bytes, label, error)) return {};
      auto object = qbfd::open(c.bytes, label);
      if (!object) {
        error = label + ": " + object.error().message;
        return {};
      }
      if (!validateLtoObject(*object->object, label, error)) return {};
    }
    out.push_back(std::move(c));
  }
  return out;
}

}  // namespace qld::detail
