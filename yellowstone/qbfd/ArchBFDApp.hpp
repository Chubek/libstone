// Archive-aware application traversal; callbacks run synchronously.
#pragma once
#include "ArchBFD.hpp"

#include <fstream>
#include <functional>

namespace archbfd {
// visitor receives (display name, const Binary&). Borrowed records/bytes must
// be copied if retained after the callback. Non-object members report an error.
// Universal Mach-O requires explicit Binary::machoSlice selection instead.
template <class Visitor>
void visitObjects(const std::filesystem::path& path, Visitor&& visitor) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw Error(ARCHBFD_IO_ERROR, "cannot read " + path.string());
  char magic[8]{};
  input.read(magic, sizeof(magic));
  if (input.bad()) throw Error(ARCHBFD_IO_ERROR, "cannot read " + path.string());
  bool archive = input.gcount() == 8 &&
      (std::string_view(magic, 8) == "!<arch>\n" ||
       std::string_view(magic, 8) == "!<thin>\n");
  input.close();
  auto name = path.string();
  if (archive) {
    auto ar = Archive::open(path);
    for (size_t i = 0, n = ar.size(); i < n; ++i) {
      auto member = ar.member(i);
      if (member.special) continue;
      auto object = ar.openMember(i);
      auto display = name + "(" + std::string(text(member.name)) + ")";
      std::invoke(visitor, std::string_view(display), std::as_const(object));
    }
  } else {
    auto object = Binary::open(path);
    std::invoke(visitor, std::string_view(name), std::as_const(object));
  }
}
} // namespace archbfd
