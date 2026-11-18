#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qdwarf {

enum class ErrorCode { Io, Malformed, Unsupported, LimitExceeded };

class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, std::string message, std::string section = {},
        std::optional<std::uint64_t> offset = {});
  ErrorCode code() const noexcept { return code_; }
  std::string_view section() const noexcept { return section_; }
  std::optional<std::uint64_t> offset() const noexcept { return offset_; }

 private:
  ErrorCode code_;
  std::string section_;
  std::optional<std::uint64_t> offset_;
};

struct Limits {
  std::size_t max_object_bytes = 128 * 1024 * 1024;
  std::size_t max_section_bytes = 64 * 1024 * 1024;
  std::size_t max_units = 64 * 1024;
  std::size_t max_line_tables = 16 * 1024;
  std::size_t max_line_entries = 256 * 1024;  // Directory and file entries combined.
  std::size_t max_line_rows = 1024 * 1024;    // Includes end-of-sequence markers.
  std::size_t max_line_operations = 4 * 1024 * 1024;
  std::size_t max_string_bytes = 4096;       // Bytes before the NUL.
  std::size_t max_string_work = 16 * 1024 * 1024; // Includes NULs and repeated reads.
};

struct Options {
  bool units = true;
  bool lines = false;
  Limits limits;
};

enum class ObjectFormat { Elf, Pe, Coff, MachO };
enum class UnitKind { LegacyInfo, Compile, Type, Partial, Skeleton, SplitCompile, SplitType };

struct UnitHeader {
  std::uint64_t offset = 0;       // Section-relative start of initial length.
  std::uint64_t unit_length = 0;  // Excludes initial length encoding.
  std::uint64_t header_size = 0;  // Includes initial length encoding.
  std::uint16_t version = 0;
  std::uint8_t offset_size = 0;
  std::uint8_t address_size = 0;
  UnitKind kind = UnitKind::LegacyInfo;
  std::uint64_t abbrev_offset = 0; // Raw, possibly unrelocated.
  std::optional<std::uint64_t> type_signature;
  std::optional<std::uint64_t> type_offset; // Unit-relative, raw.
  std::optional<std::uint64_t> dwo_id;
};

struct UnitSection {
  std::string name;
  bool has_relocation_records = false; // May be unapplied or retained after linking.
  std::vector<UnitHeader> units;
};

struct FileEntry {
  std::string path;
  std::optional<std::uint64_t> directory_index; // Encoded DWARF index, not a vector index.
};

struct LineRow {
  std::uint64_t address = 0;
  std::uint64_t file = 0; // Encoded DWARF index, not a vector index.
  std::uint64_t line = 0;
  std::uint64_t column = 0;
  std::uint64_t discriminator = 0;
  std::uint64_t isa = 0;
  std::uint8_t op_index = 0;
  bool is_stmt = false;
  bool basic_block = false;
  bool prologue_end = false;
  bool epilogue_begin = false;
  bool end_sequence = false;
};

struct LineTable {
  std::uint64_t offset = 0;
  std::uint64_t unit_length = 0;
  std::uint16_t version = 0;
  std::uint8_t offset_size = 0;
  std::uint8_t address_size = 0;
  std::vector<std::string> directories;
  std::vector<FileEntry> files;
  std::vector<LineRow> rows;

  const FileEntry* file(std::uint64_t index) const noexcept;
  // Null for absent, invalid, or pre-v5 compilation-directory (zero) indices.
  const std::string* directory(const FileEntry& file) const noexcept;
};

struct Report {
  ObjectFormat format = ObjectFormat::Elf;
  std::vector<UnitSection> unit_sections;
  std::string line_section; // Empty when absent or not requested.
  std::vector<LineTable> line_tables;
};

struct LineMatch {
  std::size_t table;
  std::size_t row;
};

// Results own all strings and records; no references to the input escape.
// Throws qdwarf::Error on malformed/unsupported selected data or a limit.
Report inspect(std::span<const std::uint8_t> object, const Options& options = {});
Report inspect_file(const std::filesystem::path& path, const Options& options = {});

// First matching half-open interval within one sequence, in section order.
// Equal-address rows are resolved by choosing the last before an advance.
std::optional<LineMatch> lookup(const Report& report, std::uint64_t address) noexcept;
std::string_view name(ObjectFormat format) noexcept;
std::string_view name(UnitKind kind) noexcept;
std::string_view name(ErrorCode code) noexcept;

} // namespace qdwarf
