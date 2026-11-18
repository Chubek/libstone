#include <qdwarf/qdwarf.hpp>

#include "qBFD.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace qdwarf {
namespace {

using Bytes = std::span<const std::uint8_t>;
using Code = ErrorCode;
using qbfd::Endian;
using u64 = std::uint64_t;
constexpr auto max64 = std::numeric_limits<u64>::max();

std::string error_text(Code code, const std::string& message, const std::string& section,
                       std::optional<u64> offset) {
  std::ostringstream out;
  out << name(code) << ": ";
  if (!section.empty()) out << section;
  if (offset) out << "+0x" << std::hex << *offset;
  if (!section.empty() || offset) out << ": ";
  out << message;
  return out.str();
}

struct Cursor {
  Bytes bytes;
  Endian endian;
  std::string_view section;
  std::size_t base = 0;
  std::size_t pos = 0;

  std::size_t offset() const { return base + pos; }
  std::size_t remaining() const { return bytes.size() - pos; }
  [[noreturn]] void fail(Code code, std::string message) const {
    fail_at(offset(), code, std::move(message));
  }
  [[noreturn]] void fail_at(std::size_t at, Code code, std::string message) const {
    throw Error(code, std::move(message), std::string(section), at);
  }
  Cursor take(u64 size) {
    if (size > remaining()) fail(Code::Malformed, "truncated field or record");
    Cursor result{bytes.subspan(pos, static_cast<std::size_t>(size)), endian, section, offset()};
    pos += static_cast<std::size_t>(size);
    return result;
  }
  std::uint8_t byte() {
    if (!remaining()) fail(Code::Malformed, "truncated byte");
    return bytes[pos++];
  }
  u64 number(unsigned width) {
    auto field = take(width).bytes;
    u64 value = 0;
    if (endian == Endian::Little) {
      for (std::size_t i = field.size(); i; --i) value = (value << 8) | field[i - 1];
    } else {
      for (auto b : field) value = (value << 8) | b;
    }
    return value;
  }
  u64 leb(bool sign = false) {
    auto start = offset();
    u64 value = 0;
    for (unsigned shift = 0; shift <= 63; shift += 7) {
      auto b = byte();
      unsigned payload = b & 0x7f;
      if (shift == 63 && ((b & 0x80) || (sign ? payload != 0 && payload != 0x7f : payload > 1)))
        fail_at(start, Code::Malformed, sign ? "SLEB128 overflow" : "ULEB128 overflow");
      value |= u64(payload) << shift;
      if (!(b & 0x80)) {
        if (sign && (b & 0x40) && shift < 63) value |= max64 << (shift + 7);
        return value;
      }
    }
    fail_at(start, Code::Malformed, "unterminated LEB128");
  }
  std::int64_t sleb() { return std::bit_cast<std::int64_t>(leb(true)); }
};

void spend(Cursor& c, std::size_t& balance, std::size_t count, const char* resource) {
  if (count > balance) c.fail(Code::LimitExceeded, std::string(resource) + " limit exceeded");
  balance -= count;
}

struct Budget {
  explicit Budget(const Limits& limits)
      : entries(limits.max_line_entries), rows(limits.max_line_rows),
        operations(limits.max_line_operations), string_work(limits.max_string_work),
        string_bytes(limits.max_string_bytes) {}
  std::size_t entries, rows, operations, string_work, string_bytes;
};

std::string read_string(Cursor& c, Budget& budget) {
  const auto start = c.offset();
  auto rest = c.bytes.subspan(c.pos);
  const auto ceiling = std::min(rest.size(), budget.string_work);
  std::size_t i = 0;
  for (; i < ceiling && i <= budget.string_bytes; ++i) {
    if (!rest[i]) {
      spend(c, budget.string_work, i + 1, "string work");
      c.pos += i + 1;
      return std::string(reinterpret_cast<const char*>(rest.data()), i);
    }
  }
  if (i > budget.string_bytes) c.fail_at(start, Code::LimitExceeded, "string length limit exceeded");
  if (i < rest.size()) c.fail_at(start, Code::LimitExceeded, "string work limit exceeded");
  c.fail_at(start, Code::Malformed, "unterminated string");
}

struct Record {
  Cursor body;
  u64 length;
  std::uint8_t offset_size;
};

Record record(Cursor& c) {
  auto start = c.offset();
  auto length = c.number(4);
  std::uint8_t offset_size = 4;
  if (length == 0xffffffff) {
    offset_size = 8;
    length = c.number(8);
  } else if (length >= 0xfffffff0) {
    c.fail_at(start, Code::Unsupported, "reserved DWARF initial length");
  }
  if (!length) c.fail_at(start, Code::Malformed, "zero DWARF unit length");
  return {c.take(length), length, offset_size};
}

std::uint16_t version(Cursor& c) {
  auto start = c.offset();
  auto v = c.number(2);
  if (v < 2 || v > 5)
    c.fail_at(start, Code::Unsupported, "DWARF version " + std::to_string(v));
  return static_cast<std::uint16_t>(v);
}

std::uint8_t address_size(Cursor& c, unsigned size, std::size_t offset) {
  if (size != 1 && size != 2 && size != 4 && size != 8)
    c.fail_at(offset, Code::Unsupported, "address size " + std::to_string(size));
  return static_cast<std::uint8_t>(size);
}

std::uint8_t read_address_size(Cursor& c) {
  auto offset = c.offset();
  return address_size(c, c.byte(), offset);
}

UnitHeader unit(Cursor& section, bool types) {
  UnitHeader header;
  header.offset = section.offset();
  auto [c, length, offset_size] = record(section);
  header.unit_length = length;
  header.offset_size = offset_size;
  header.version = version(c);
  if (types && header.version != 4)
    c.fail_at(c.base, Code::Unsupported, ".debug_types is supported only in DWARF 4");
  if (header.version == 5) {
    switch (auto kind = c.byte()) {
      case 1: header.kind = UnitKind::Compile; break;
      case 2: header.kind = UnitKind::Type; break;
      case 3: header.kind = UnitKind::Partial; break;
      case 4: header.kind = UnitKind::Skeleton; break;
      case 5: header.kind = UnitKind::SplitCompile; break;
      case 6: header.kind = UnitKind::SplitType; break;
      default:
        c.fail_at(c.offset() - 1, Code::Unsupported, "DWARF unit type " + std::to_string(kind));
    }
    header.address_size = read_address_size(c);
    header.abbrev_offset = c.number(offset_size);
  } else {
    header.kind = types ? UnitKind::Type : UnitKind::LegacyInfo;
    header.abbrev_offset = c.number(offset_size);
    header.address_size = read_address_size(c);
  }
  if (header.kind == UnitKind::Type || header.kind == UnitKind::SplitType) {
    header.type_signature = c.number(8);
    header.type_offset = c.number(offset_size);
  }
  if (header.kind == UnitKind::Skeleton || header.kind == UnitKind::SplitCompile)
    header.dwo_id = c.number(8);
  header.header_size = c.offset() - header.offset;
  return header;
}

enum class Slot { Info, Types, Line, LineStr, Str, Count };

struct Located {
  const qbfd::Section* section = nullptr;
  bool split = false;
  bool compressed = false;
  bool relocated = false;
  bool duplicate = false;
};

struct Sections {
  const qbfd::ObjectFile& object;
  const Limits& limits;
  std::array<Located, static_cast<std::size_t>(Slot::Count)> slots{};

  const Located& operator[](Slot slot) const { return slots[static_cast<std::size_t>(slot)]; }

  Sections(const qbfd::ObjectFile& obj, const Options& options) : object(obj), limits(options.limits) {
    const bool macho = obj.format() == qbfd::Format::MachO;
    for (const auto& sec : obj.sections()) {
      std::string_view n = sec.name;
      if (macho) {
        if (!n.starts_with("__")) continue;
        n.remove_prefix(2);
      } else {
        if (!n.starts_with('.')) continue;
        n.remove_prefix(1);
      }
      const bool compressed = n.starts_with("zdebug_")
          || (obj.format() == qbfd::Format::ELF && (sec.nativeFlags & 0x800)); // SHF_COMPRESSED
      if (n.starts_with("zdebug_")) n.remove_prefix(1);
      const bool split = n.ends_with(".dwo");
      if (split) n.remove_suffix(4);
      Slot slot;
      if (n == "debug_info" && options.units) slot = Slot::Info;
      else if (n == "debug_types" && options.units) slot = Slot::Types;
      else if (n == "debug_line" && options.lines) slot = Slot::Line;
      else if ((n == "debug_line_str" || (macho && compressed && n == "debug_line_st")) && options.lines)
        slot = Slot::LineStr;
      else if (n == "debug_str" && options.lines) slot = Slot::Str;
      else continue;
      // A regular section takes precedence over an unrelated split section.
      auto& found = slots[static_cast<std::size_t>(slot)];
      if (found.section) {
        if (split && !found.split) continue;
        if (split == found.split) {
          found.duplicate = true;
          continue;
        }
      }
      found = {&sec, split, compressed, bool(sec.flags & qbfd::sec::Reloc), false};
    }
    if (obj.format() == qbfd::Format::ELF) {
      // qBFD exposes ELF relocation-target indices without copying relocation records.
      const auto& elf = static_cast<const qbfd::elf::ELFFile&>(obj);
      for (const auto& raw : elf.rawSections()) {
        if ((raw.type != 4 && raw.type != 9) || !raw.size || !raw.info) continue;
        for (auto& loc : slots)
          if (loc.section && loc.section->index == raw.info) loc.relocated = true;
      }
    } else if (obj.format() == qbfd::Format::PE) {
      const auto& pe = static_cast<const qbfd::pe::PEFile&>(obj);
      for (auto& loc : slots) {
        if (!loc.section) continue;
        // PE base relocations are for rebasing a linked image at runtime.
        // Source lookup uses linked addresses at the preferred image base.
        loc.relocated = pe.relocationCount(loc.section->index) != 0;
      }
    }
  }

  Cursor open(Slot slot, bool require_unrelocated) const {
    const auto& loc = (*this)[slot];
    const auto& sec = *loc.section;
    if (loc.duplicate)
      throw Error(Code::Unsupported, "multiple sections for the same DWARF data", sec.name, 0);
    if (loc.compressed) throw Error(Code::Unsupported, "compressed DWARF section", sec.name, 0);
    if (loc.split) throw Error(Code::Unsupported, "split DWARF section", sec.name, 0);
    if (require_unrelocated && loc.relocated)
      throw Error(Code::Unsupported,
                  "relocation records in line data; use a linked image without retained debug relocations",
                  sec.name, 0);
    if ((sec.flags & qbfd::sec::Bss) || sec.size != sec.fileSize)
      throw Error(Code::Malformed, "DWARF section is not entirely file-backed", sec.name, 0);
    if (sec.fileSize > limits.max_section_bytes)
      throw Error(Code::LimitExceeded, "DWARF section byte limit exceeded", sec.name, 0);
    auto contents = object.sectionContents(sec);
    if (!contents) throw Error(Code::Malformed, contents.error().message, sec.name, 0);
    return {*contents, object.endian(), sec.name};
  }
};

struct Descriptor {
  u64 content;
  u64 form;
};

struct FormValue {
  std::optional<std::string> text;
  std::optional<u64> number;
};

class LineParser {
 public:
  LineParser(const Sections& sections, Budget& budget)
      : sections_(sections), budget_(budget) {}

  LineTable parse(Cursor& section) {
    table_.offset = section.offset();
    auto [c, length, offset_size] = record(section);
    table_.unit_length = length;
    table_.offset_size = offset_size;
    table_.version = version(c);
    if (table_.version == 5) {
      table_.address_size = read_address_size(c);
      if (c.byte())
        c.fail_at(c.offset() - 1, Code::Unsupported, "segmented line tables");
    }
    auto header_size = c.number(offset_size);
    auto header = c.take(header_size);
    minimum_ = header.byte();
    max_ops_ = table_.version >= 4 ? header.byte() : 1;
    auto stmt = header.byte();
    default_stmt_ = stmt != 0;
    auto line_base = header.byte();
    line_base_ = line_base >= 128 ? int(line_base) - 256 : line_base;
    line_range_ = header.byte();
    opcode_base_ = header.byte();
    if (!minimum_ || !max_ops_ || !line_range_ || !opcode_base_ || stmt > 1)
      header.fail_at(header.base, Code::Malformed, "invalid line machine parameters");
    auto lengths = header.take(opcode_base_ - 1).bytes;
    constexpr std::array<std::uint8_t, 12> counts{0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    for (std::size_t i = 0; i < std::min(lengths.size(), counts.size()); ++i)
      if (lengths[i] != counts[i])
        header.fail_at(header.offset() - lengths.size() + i, Code::Malformed,
                       "invalid standard opcode operand count");
    if (table_.version == 5) {
      read_v5_entries(header, false);
      read_v5_entries(header, true);
    } else {
      read_legacy_entries(header);
    }
    if (header.remaining()) header.fail(Code::Malformed, "unexpected bytes in line prologue");
    run(c);
    return std::move(table_);
  }

 private:
  std::string string_at(Cursor& field, Slot slot, u64 offset) {
    auto& cached = slot == Slot::LineStr ? line_strings_ : strings_;
    if (!cached) {
      if (!sections_[slot].section)
        field.fail(Code::Malformed, slot == Slot::LineStr ? "missing .debug_line_str" : "missing .debug_str");
      cached = sections_.open(slot, true);
    }
    auto c = *cached;
    if (offset >= c.bytes.size()) field.fail(Code::Malformed, "string offset is outside its section");
    c.pos = static_cast<std::size_t>(offset);
    return read_string(c, budget_);
  }

  FormValue form(Cursor& c, u64 encoding) {
    switch (encoding) {
      case 0x03: { auto len = c.number(2); c.take(len); return {}; } // DW_FORM_block2
      case 0x04: { auto len = c.number(4); c.take(len); return {}; } // DW_FORM_block4
      case 0x05: return {{}, c.number(2)}; // DW_FORM_data2
      case 0x06: return {{}, c.number(4)}; // DW_FORM_data4
      case 0x07: return {{}, c.number(8)}; // DW_FORM_data8
      case 0x08: return {read_string(c, budget_), {}}; // DW_FORM_string
      case 0x09: { auto len = c.leb(); c.take(len); return {}; } // DW_FORM_block
      case 0x0a: { auto len = c.byte(); c.take(len); return {}; } // DW_FORM_block1
      case 0x0b: return {{}, c.byte()}; // DW_FORM_data1
      case 0x0d: { // DW_FORM_sdata
        auto n = c.sleb();
        if (n < 0) return {};
        return {{}, static_cast<u64>(n)};
      }
      case 0x0e: { // DW_FORM_strp
        auto offset = c.number(table_.offset_size);
        return {string_at(c, Slot::Str, offset), {}};
      }
      case 0x0f: return {{}, c.leb()}; // DW_FORM_udata
      case 0x1e: c.take(16); return {}; // DW_FORM_data16
      case 0x1f: { // DW_FORM_line_strp
        auto offset = c.number(table_.offset_size);
        return {string_at(c, Slot::LineStr, offset), {}};
      }
      default: c.fail(Code::Unsupported, "line-table form " + std::to_string(encoding));
    }
  }

  void check_directory(Cursor& c, const FileEntry& file) const {
    if (!file.directory_index) return;
    if (table_.version < 5 && *file.directory_index == 0) return;
    if (!table_.directory(file)) c.fail(Code::Malformed, "directory index is outside the table");
  }

  void read_v5_entries(Cursor& c, bool files) {
    std::vector<Descriptor> descriptors;
    auto count = c.byte();
    descriptors.reserve(count);
    bool path = false, dir = false;
    for (unsigned i = 0; i < count; ++i) {
      Descriptor d{c.leb(), c.leb()};
      if (d.content == 1 && std::exchange(path, true))
        c.fail(Code::Malformed, "duplicate path descriptor");
      if (d.content == 2 && std::exchange(dir, true))
        c.fail(Code::Malformed, "duplicate directory-index descriptor");
      descriptors.push_back(d);
    }
    auto entries = c.leb();
    if (!path && entries) c.fail(Code::Malformed, "missing path descriptor");
    if (entries > budget_.entries) c.fail(Code::LimitExceeded, "line entry limit exceeded");
    spend(c, budget_.entries, static_cast<std::size_t>(entries), "line entry");
    // Every supported form consumes at least one byte.
    if (count && entries > c.remaining() / count)
      c.fail(Code::Malformed, "truncated line entries");
    for (u64 i = 0; i < entries; ++i) {
      FileEntry entry;
      for (auto d : descriptors) {
        auto value = form(c, d.form);
        if (d.content == 1) { // DW_LNCT_path
          if (!value.text) c.fail(Code::Malformed, "path does not use a string form");
          entry.path = std::move(*value.text);
        } else if (d.content == 2) { // DW_LNCT_directory_index
          if (!value.number) c.fail(Code::Malformed, "directory index is not an unsigned value");
          entry.directory_index = value.number;
        }
      }
      if (files) {
        check_directory(c, entry);
        table_.files.push_back(std::move(entry));
      } else {
        table_.directories.push_back(std::move(entry.path));
      }
    }
  }

  void legacy_file(Cursor& c, std::string path) {
    spend(c, budget_.entries, 1, "line entry");
    FileEntry entry{std::move(path), c.leb()};
    c.leb(); // timestamp
    c.leb(); // file size
    check_directory(c, entry);
    table_.files.push_back(std::move(entry));
  }

  void read_legacy_entries(Cursor& c) {
    while (true) {
      auto path = read_string(c, budget_);
      if (path.empty()) break;
      spend(c, budget_.entries, 1, "line entry");
      table_.directories.push_back(std::move(path));
    }
    while (true) {
      auto path = read_string(c, budget_);
      if (path.empty()) break;
      legacy_file(c, std::move(path));
    }
  }

  void reset() {
    state_ = {};
    state_.file = 1;
    state_.line = 1;
    state_.is_stmt = default_stmt_;
    previous_.reset();
    active_ = false;
  }

  u64 address_max() const {
    return !table_.address_size || table_.address_size == 8
        ? max64 : (u64(1) << (table_.address_size * 8)) - 1;
  }

  void advance_address(Cursor& c, u64 steps, unsigned scale) {
    const auto max = address_max();
    if (state_.address > max || steps > (max - state_.address) / scale)
      c.fail(Code::Malformed, "line address overflow");
    state_.address += steps * scale;
    if (!table_.address_size) inferred_peak_ = std::max(inferred_peak_, state_.address);
  }

  void advance_ops(Cursor& c, u64 ops) {
    // Factor out the quotient before adding the small operation index.
    const auto rest = ops % max_ops_ + state_.op_index;
    advance_address(c, ops / max_ops_ + rest / max_ops_, minimum_);
    state_.op_index = static_cast<std::uint8_t>(rest % max_ops_);
  }

  void advance_line(Cursor& c, std::int64_t delta) {
    if (delta < 0) {
      const auto magnitude = u64(0) - static_cast<u64>(delta);
      if (magnitude > state_.line) c.fail(Code::Malformed, "line number underflow");
      state_.line -= magnitude;
    } else {
      if (static_cast<u64>(delta) > max64 - state_.line) c.fail(Code::Malformed, "line number overflow");
      state_.line += static_cast<u64>(delta);
    }
  }

  void emit(Cursor& c, bool end = false) {
    if (!end && !table_.file(state_.file))
      c.fail(Code::Malformed, "file index is outside the line table");
    if (previous_ && (state_.address < previous_->first
        || (state_.address == previous_->first && state_.op_index < previous_->second)))
      c.fail(Code::Malformed, "line addresses decrease within a sequence");
    spend(c, budget_.rows, 1, "line row");
    state_.end_sequence = end;
    table_.rows.push_back(state_);
    previous_ = {state_.address, state_.op_index};
    state_.basic_block = state_.prologue_end = state_.epilogue_begin = false;
    state_.discriminator = 0;
  }

  void extended(Cursor& c) {
    const auto length = c.leb();
    auto op = c.take(length);
    switch (auto opcode = op.byte()) {
      case 1: // DW_LNE_end_sequence
        emit(op, true);
        reset();
        break;
      case 2: { // DW_LNE_set_address
        if (!table_.address_size) {
          if (op.remaining() > 8) op.fail(Code::Unsupported, "unsupported set_address operand width");
          table_.address_size = address_size(op, static_cast<unsigned>(op.remaining()), op.offset());
          if (inferred_peak_ > address_max())
            op.fail(Code::Malformed, "line address exceeds inferred width");
        }
        state_.address = op.number(table_.address_size);
        state_.op_index = 0;
        break;
      }
      case 3: { // DW_LNE_define_file (legacy encoding)
        if (table_.version >= 5) op.fail(Code::Unsupported, "DW_LNE_define_file in DWARF 5");
        auto path = read_string(op, budget_);
        if (path.empty()) op.fail(Code::Malformed, "empty DW_LNE_define_file path");
        legacy_file(op, std::move(path));
        break;
      }
      case 4: state_.discriminator = op.leb(); break; // DW_LNE_set_discriminator
      default: op.fail(Code::Unsupported, "extended line opcode " + std::to_string(opcode));
    }
    if (op.remaining()) op.fail(Code::Malformed, "unexpected extended opcode payload");
  }

  void run(Cursor& c) {
    reset();
    while (c.remaining()) {
      spend(c, budget_.operations, 1, "line operation");
      auto start = c.offset();
      auto opcode = c.byte();
      active_ = true;
      if (!opcode) {
        extended(c);
      } else if (opcode >= opcode_base_) {
        unsigned adjusted = opcode - opcode_base_;
        advance_ops(c, adjusted / line_range_);
        advance_line(c, line_base_ + static_cast<int>(adjusted % line_range_));
        emit(c);
      } else {
        switch (opcode) {
          case 1: emit(c); break;
          case 2: advance_ops(c, c.leb()); break;
          case 3: advance_line(c, c.sleb()); break;
          case 4: state_.file = c.leb(); break;
          case 5: state_.column = c.leb(); break;
          case 6: state_.is_stmt = !state_.is_stmt; break;
          case 7: state_.basic_block = true; break;
          case 8: advance_ops(c, (255u - opcode_base_) / line_range_); break;
          case 9:
            advance_address(c, c.number(2), 1);
            state_.op_index = 0;
            break;
          case 10: state_.prologue_end = true; break;
          case 11: state_.epilogue_begin = true; break;
          case 12: state_.isa = c.leb(); break;
          default: c.fail_at(start, Code::Unsupported, "standard line opcode " + std::to_string(opcode));
        }
      }
    }
    if (active_) c.fail(Code::Malformed, "line sequence is missing DW_LNE_end_sequence");
    if (!table_.address_size) {
      table_.address_size = sections_.object.is64Bit() ? 8 : 4;
      if (inferred_peak_ > address_max())
        c.fail(Code::Malformed, "line address exceeds object address width");
    }
  }

  const Sections& sections_;
  Budget& budget_;
  LineTable table_;
  std::optional<Cursor> line_strings_, strings_;
  unsigned minimum_ = 0, max_ops_ = 0, line_range_ = 0, opcode_base_ = 0;
  u64 inferred_peak_ = 0;
  int line_base_ = 0;
  bool default_stmt_ = false, active_ = false;
  LineRow state_;
  std::optional<std::pair<u64, std::uint8_t>> previous_;
};

Report parse_object(const qbfd::ObjectFile& obj, const Options& options) {
  ObjectFormat format;
  switch (obj.format()) {
    case qbfd::Format::ELF: format = ObjectFormat::Elf; break;
    case qbfd::Format::PE: format = ObjectFormat::Pe; break;
    case qbfd::Format::COFF: format = ObjectFormat::Coff; break;
    case qbfd::Format::MachO: format = ObjectFormat::MachO; break;
    default: throw Error(Code::Unsupported, "unexpected object format");
  }
  Report result{format, {}, {}, {}};
  Sections sections(obj, options);
  auto units_left = options.limits.max_units;
  for (auto slot : {Slot::Info, Slot::Types}) {
    if (!sections[slot].section) continue;
    auto cursor = sections.open(slot, false);
    UnitSection group{std::string(cursor.section), sections[slot].relocated, {}};
    while (cursor.remaining()) {
      spend(cursor, units_left, 1, "DWARF unit");
      group.units.push_back(unit(cursor, slot == Slot::Types));
    }
    result.unit_sections.push_back(std::move(group));
  }
  if (sections[Slot::Line].section) {
    auto cursor = sections.open(Slot::Line, true);
    result.line_section = cursor.section;
    Budget budget(options.limits);
    auto tables_left = options.limits.max_line_tables;
    while (cursor.remaining()) {
      spend(cursor, tables_left, 1, "line table");
      result.line_tables.push_back(LineParser(sections, budget).parse(cursor));
    }
  }
  return result;
}

Report inspect_bytes(Bytes bytes, const std::filesystem::path& label, const Options& options) {
  if (qbfd::macho::isFat(bytes))
    throw Error(Code::Unsupported, "universal Mach-O; extract the desired slice first");
  auto opened = [&]() -> qbfd::Expected<std::unique_ptr<qbfd::ObjectFile>> {
    // Select a builtin explicitly; section classification uses its native metadata.
    switch (qbfd::detectFormat(bytes)) {
      case qbfd::Format::ELF: return qbfd::elf::ELFFile::open(bytes, label);
      case qbfd::Format::PE: return qbfd::pe::PEFile::open(bytes, label);
      case qbfd::Format::COFF: return qbfd::coff::COFFFile::open(bytes, label);
      case qbfd::Format::MachO: return qbfd::macho::MachOFile::open(bytes, label);
      default: throw Error(Code::Unsupported, "expected ELF, PE/COFF, or thin Mach-O");
    }
  }();
  if (!opened) {
    auto& e = opened.error();
    const auto code = e.code == qbfd::Error::Code::UnknownFormat || e.code == qbfd::Error::Code::Unsupported
        ? Code::Unsupported : Code::Malformed;
    throw Error(code, "qBFD: " + e.message);
  }
  return parse_object(**opened, options);
}

} // namespace

Error::Error(ErrorCode code, std::string message, std::string section, std::optional<u64> offset)
    : std::runtime_error(error_text(code, message, section, offset)),
      code_(code), section_(std::move(section)), offset_(offset) {}

Report inspect(Bytes object, const Options& options) {
  if (object.size() > options.limits.max_object_bytes)
    throw Error(Code::LimitExceeded, "object byte limit exceeded");
  return inspect_bytes(object, {}, options);
}

Report inspect_file(const std::filesystem::path& path, const Options& options) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    throw Error(Code::Io, "input is not a readable regular file");
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw Error(Code::Io, "cannot open input file");
  const auto end = input.tellg();
  if (end < 0) throw Error(Code::Io, "cannot determine input file size");
  const auto size = static_cast<std::uintmax_t>(static_cast<std::streamoff>(end));
  if (size > options.limits.max_object_bytes
      || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
    throw Error(Code::LimitExceeded, "object byte limit exceeded");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!input.seekg(0) || (size && !input.read(reinterpret_cast<char*>(bytes.data()),
                                            static_cast<std::streamsize>(size))))
    throw Error(Code::Io, "cannot read complete input file");
  return inspect_bytes(bytes, path, options);
}

const FileEntry* LineTable::file(u64 index) const noexcept {
  if (version < 5) {
    if (index == 0) return nullptr;
    --index;
  }
  return index < files.size() ? &files[static_cast<std::size_t>(index)] : nullptr;
}

const std::string* LineTable::directory(const FileEntry& file_entry) const noexcept {
  if (!file_entry.directory_index) return nullptr;
  auto index = *file_entry.directory_index;
  if (version < 5) {
    if (index == 0) return nullptr;
    --index;
  }
  return index < directories.size() ? &directories[static_cast<std::size_t>(index)] : nullptr;
}

std::optional<LineMatch> lookup(const Report& report, u64 address) noexcept {
  for (std::size_t ti = 0; ti < report.line_tables.size(); ++ti) {
    const auto& rows = report.line_tables[ti].rows;
    for (std::size_t i = 1; i < rows.size(); ++i)
      if (!rows[i - 1].end_sequence && rows[i - 1].address <= address && address < rows[i].address)
        return LineMatch{ti, i - 1};
  }
  return {};
}

std::string_view name(ObjectFormat format) noexcept {
  switch (format) {
    case ObjectFormat::Elf: return "ELF";
    case ObjectFormat::Pe: return "PE";
    case ObjectFormat::Coff: return "COFF";
    case ObjectFormat::MachO: return "Mach-O";
  }
  return "unknown object format";
}

std::string_view name(UnitKind kind) noexcept {
  switch (kind) {
    case UnitKind::LegacyInfo: return "legacy-info";
    case UnitKind::Compile: return "compile";
    case UnitKind::Type: return "type";
    case UnitKind::Partial: return "partial";
    case UnitKind::Skeleton: return "skeleton";
    case UnitKind::SplitCompile: return "split-compile";
    case UnitKind::SplitType: return "split-type";
  }
  return "unknown unit type";
}

std::string_view name(ErrorCode code) noexcept {
  switch (code) {
    case Code::Io: return "I/O error";
    case Code::Malformed: return "malformed data";
    case Code::Unsupported: return "unsupported data";
    case Code::LimitExceeded: return "resource limit";
  }
  return "error";
}

} // namespace qdwarf
