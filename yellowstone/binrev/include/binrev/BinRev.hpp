#pragma once

#include <qBFDCore.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace binrev {

inline constexpr uint64_t maxInputBytes = 256ull * 1024 * 1024;

struct Address {
  uint64_t value;  // Preferred linked virtual address, not a runtime VA or RVA.
};
struct FileOffset {
  uint64_t value;
};
struct SectionOffset {
  uint32_t index;  // Canonical qbfd::Section::index.
  uint64_t offset;
};
using Position = std::variant<Address, FileOffset, SectionOffset>;

struct SymbolMatch {
  qbfd::Symbol symbol;
  bool dynamic;          // true if the symbol is from dynamicSymbols().
  uint64_t displacement; // Bytes from the symbol's start.
};

struct Location {
  std::optional<Address> address;       // Absent for relocatable/non-allocated data.
  std::optional<FileOffset> fileOffset; // Absent for BSS and other unbacked data.
  std::optional<SectionOffset> section; // Absent for raw offsets outside sections.
  std::optional<SymbolMatch> symbol;
};

// A move-only read-only view of an object file, with ownership of the input.
class Binary {
 public:
  // Path inputs must be regular files. Both overloads enforce maxInputBytes.
  static qbfd::Expected<Binary> open(const std::filesystem::path& path);
  static qbfd::Expected<Binary> open(std::vector<uint8_t> bytes,
                                     const std::filesystem::path& label = {});

  // Canonical parsed metadata (including sections, symbols and relocations).
  // The reference is borrowed, like the byte span returned by readBytes().
  const qbfd::ObjectFile& object() const { return *object_; }

  // Resolve one byte to its section, file/address coordinates and symbol if
  // available. Location is an owned result. Ambiguous overlaps are errors.
  qbfd::Expected<Location> locate(Position position) const;

  // Read a non-empty range. Address and section reads must stay in one
  // file-backed section; raw file-offset reads may include headers and gaps.
  // The view is valid only while this Binary (or the one it moves to) owns it.
  qbfd::Expected<std::span<const uint8_t>> readBytes(Position position,
                                                     uint64_t length) const;

 private:
  explicit Binary(std::unique_ptr<qbfd::ObjectFile> object);
  std::unique_ptr<qbfd::ObjectFile> object_;
};

}  // namespace binrev
