#include <qdwarf/qdwarf.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <source_location>
#include <utility>

namespace {

using qdwarf::ErrorCode;
using qdwarf::ObjectFormat;
using u64 = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;

void check(bool condition, const std::source_location at = std::source_location::current()) {
  if (!condition) {
    std::cerr << at.file_name() << ':' << at.line() << ": assertion failed\n";
    std::exit(1);
  }
}

struct Writer {
  Bytes data;
  bool big = false;
  void set(std::size_t offset, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
      data.at(offset + (big ? width - 1 - i : i)) = static_cast<std::uint8_t>(value);
      value >>= 8;
    }
  }
  void add(u64 value, unsigned width) {
    auto start = data.size();
    data.resize(start + width);
    set(start, value, width);
  }
  void bytes(std::initializer_list<std::uint8_t> values) { data.insert(data.end(), values); }
  void append(const Bytes& values) { data.insert(data.end(), values.begin(), values.end()); }
  void string(std::string_view value) {
    data.insert(data.end(), value.begin(), value.end());
    data.push_back(0);
  }
};

struct Section {
  std::string name;
  Bytes data;
  unsigned type = 1, flags = 0, link = 0, info = 0, entsize = 0;
};

Bytes elf(const std::vector<Section>& sections, bool big = false) {
  const std::size_t n = sections.size() + 2;
  Writer out{Bytes(64 + n * 64), big};
  out.set(0, big ? 0x7f454c46 : 0x464c457f, 4);
  out.set(4, 2, 1);
  out.set(5, big ? 2 : 1, 1);
  out.set(6, 1, 1);
  out.set(16, 2, 2);
  out.set(18, big ? 21 : 62, 2);
  out.set(20, 1, 4);
  out.set(40, 64, 8);
  out.set(52, 64, 2);
  out.set(58, 64, 2);
  out.set(60, n, 2);
  out.set(62, n - 1, 2);
  Writer names{{0}};
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const auto& s = sections[i];
    auto p = 64 * (i + 2);
    out.set(p, names.data.size(), 4);
    names.string(s.name);
    out.set(p + 4, s.type, 4);
    out.set(p + 8, s.flags, 8);
    out.set(p + 24, out.data.size(), 8);
    out.set(p + 32, s.data.size(), 8);
    out.set(p + 40, s.link, 4);
    out.set(p + 44, s.info, 4);
    out.set(p + 48, 1, 8);
    out.set(p + 56, s.entsize, 8);
    out.append(s.data);
  }
  auto last = 64 * n;
  out.set(last, names.data.size(), 4);
  names.string(".shstrtab");
  out.set(last + 4, 3, 4);
  out.set(last + 24, out.data.size(), 8);
  out.set(last + 32, names.data.size(), 8);
  out.set(last + 48, 1, 8);
  out.append(names.data);
  return out.data;
}

Bytes windows(const std::vector<Section>& sections, bool pe) {
  std::size_t fh = pe ? 68 : 0, oh = fh + 20, sh = oh + (pe ? 112 : 0);
  Writer out{Bytes(sh + sections.size() * 40)};
  if (pe) {
    out.set(0, 0x5a4d, 2); // MZ
    out.set(0x3c, fh - 4, 4);
    out.set(fh - 4, 0x4550, 4); // PE signature
    out.set(fh + 16, 112, 2);
    out.set(fh + 18, 2, 2);
    out.set(oh, 0x20b, 2);
    out.set(oh + 24, 0x140000000, 8);
    out.set(oh + 32, 0x1000, 4);
    out.set(oh + 36, 0x200, 4);
  }
  out.set(fh, 0x8664, 2);
  out.set(fh + 2, sections.size(), 2);
  Writer names{Bytes(4)};
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const auto& s = sections[i];
    auto p = sh + 40 * i;
    auto ref = '/' + std::to_string(names.data.size());
    for (std::size_t j = 0; j < ref.size(); ++j) out.set(p + j, ref[j], 1);
    names.string(s.name);
    const auto raw_size = s.data.size() + (pe ? 16 : 0);
    out.set(p + 8, pe ? s.data.size() : 0, 4);
    out.set(p + 12, pe ? 0x1000 * (i + 1) : 0, 4);
    out.set(p + 16, raw_size, 4);
    out.set(p + 20, out.data.size(), 4);
    out.set(p + 36, 0x42000040, 4);
    auto end = out.data.size() + raw_size;
    out.append(s.data);
    out.data.resize(end);
  }
  out.set(fh + 8, out.data.size(), 4);
  names.set(0, names.data.size(), 4);
  out.append(names.data);
  return out.data;
}

Bytes macho(const std::vector<Section>& sections) {
  auto header_end = 32 + 72 + 80 * sections.size();
  Writer out{Bytes(header_end)};
  out.set(0, 0xfeedfacf, 4);
  out.set(4, 0x01000007, 4);
  out.set(8, 3, 4);
  out.set(12, 0xa, 4); // MH_DSYM
  out.set(16, 1, 4);
  out.set(20, header_end - 32, 4);
  out.set(32, 0x19, 4);
  out.set(36, header_end - 32, 4);
  constexpr std::string_view segment = "__DWARF";
  for (std::size_t j = 0; j < segment.size(); ++j) out.set(40 + j, segment[j], 1);
  out.set(72, header_end, 8);
  out.set(96, sections.size(), 4);
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const auto& s = sections[i];
    auto p = 104 + 80 * i;
    auto name = '_' + s.name;
    name[1] = '_';
    for (std::size_t j = 0; j < name.size(); ++j) out.set(p + j, name[j], 1);
    for (std::size_t j = 0; j < segment.size(); ++j) out.set(p + 16 + j, segment[j], 1);
    out.set(p + 40, s.data.size(), 8);
    out.set(p + 48, out.data.size(), 4);
    out.set(p + 64, 0x02000000, 4);
    out.append(s.data);
  }
  out.set(64, out.data.size() - header_end, 8);
  out.set(80, out.data.size() - header_end, 8);
  return out.data;
}

Bytes record(const Writer& body, bool dwarf64) {
  Writer out{{}, body.big};
  if (dwarf64) out.add(0xffffffff, 4);
  out.add(body.data.size(), dwarf64 ? 8 : 4);
  out.append(body.data);
  return out.data;
}

Bytes unit(unsigned version, bool big = false, bool dwarf64 = false,
           unsigned kind = 1, bool types = false) {
  Writer body{{}, big};
  body.add(version, 2);
  if (version == 5) body.bytes({static_cast<std::uint8_t>(kind), 8});
  body.add(dwarf64 ? 0x1122334455667788 : 0x11223344, dwarf64 ? 8 : 4);
  if (version != 5) body.bytes({4});
  if (kind == 2 || kind == 6 || types) {
    body.add(0x1122334455667788, 8);
    body.add(40, dwarf64 ? 8 : 4);
  } else if (kind == 4 || kind == 5) {
    body.add(0x1122334455667788, 8);
  }
  return record(body, dwarf64);
}

void set_address(Writer& out, u64 address, std::uint8_t width = 4) {
  out.bytes({0, static_cast<std::uint8_t>(width + 1), 2});
  out.add(address, width);
}

Bytes program(bool v5, bool big = false) {
  Writer p{{}, big};
  set_address(p, 0x1000);
  if (v5) p.bytes({4, 0}); // DW_LNS_set_file (DWARF 5 indices start at zero)
  p.bytes({5, 7, 7, 10, 12, 2, 0, 2, 4, 3, 1}); // column, flags, discriminator, copy
  p.bytes({3, 2, 2, 1, 1}); // line +2; v4+ op_index to 1; copy (same byte address)
  p.bytes({38}); // special opcode: operation +2, line +0
  p.bytes({6, 11, 9}); // negate is_stmt, epilogue_begin, fixed advance
  p.add(4, 2);
  p.bytes({1, 2, 4, 0, 1, 1}); // copy; advance; end sequence
  set_address(p, 0x2000);
  if (v5) {
    p.bytes({4, 0});
  } else {
    p.bytes({0, 10, 3, 'n', 'e', 'w', '.', 'c', 0, 0, 0, 0, 4, 2}); // define_file; set_file
  }
  p.bytes({1, 2, 2, 0, 1, 1});
  return p.data;
}

Bytes line(unsigned version, const Bytes& ops, bool big = false, bool dwarf64 = false,
           unsigned width = 4, unsigned segment = 0, unsigned form = 0x1f,
           u64 str_offset = 0) {
  Writer header{{}, big};
  header.bytes({2}); // minimum_instruction_length
  if (version >= 4) header.bytes({2}); // maximum_operations_per_instruction
  header.bytes({1, 251, 10, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1});
  if (version < 5) {
    header.string("src");
    header.bytes({0});
    header.string("a.c");
    header.bytes({1, 0, 0, 0});
  } else {
    header.bytes({1, 1, static_cast<std::uint8_t>(form), 1}); // one directory path
    header.add(str_offset, dwarf64 ? 8 : 4);
    header.bytes({3, 1, 0x0e, 2, 0x0f, 5, 0x1e, 1}); // path, dir index, MD5; one file
    header.add(0, dwarf64 ? 8 : 4); // offset into .debug_str
    header.bytes({0}); // directory index
    header.data.resize(header.data.size() + 16);
  }
  Writer body{{}, big};
  body.add(version, 2);
  if (version == 5) {
    body.add(width, 1);
    body.add(segment, 1);
  }
  body.add(header.data.size(), dwarf64 ? 8 : 4);
  body.append(header.data);
  body.append(ops);
  return record(body, dwarf64);
}

qdwarf::Options line_options() {
  qdwarf::Options opts;
  opts.lines = true;
  return opts;
}

std::vector<Section> legacy_sections(unsigned version = 4, bool big = false) {
  return {{".debug_info", unit(version, big)},
          {".debug_line", line(version, program(false, big), big)}};
}

void line_result(const qdwarf::Report& report, unsigned version) {
  check(report.line_tables.size() == 1);
  const auto& t = report.line_tables[0];
  check(t.version == version && t.address_size == 4 && t.rows.size() == 7);
  check(t.files.size() == (version == 5 ? 1 : 2));
  check(t.rows[0].address == 0x1000 && t.rows[0].line == 1 && t.rows[0].column == 7);
  check(t.rows[0].is_stmt && t.rows[0].prologue_end && t.rows[0].basic_block);
  check(t.rows[0].isa == 2 && t.rows[0].discriminator == 3);
  check(t.rows[1].discriminator == 0 && !t.rows[1].prologue_end && !t.rows[1].basic_block);
  check(t.rows[1].line == 3);
  check(t.rows[3].epilogue_begin && !t.rows[3].is_stmt && !t.rows[4].epilogue_begin);
  check(t.rows[4].end_sequence && t.rows[6].end_sequence);
  check(t.rows[5].address == 0x2000 && t.rows[5].line == 1 && t.rows[5].isa == 0 && t.rows[5].is_stmt);
  auto first = t.file(t.rows[0].file);
  check(first && first->path == "a.c");
  auto dir = t.directory(*first);
  check(dir && *dir == "src");
  auto last = t.file(t.rows[5].file);
  check(last && last->path == (version == 5 ? "a.c" : "new.c"));
  auto matches_row = [&](u64 address, std::size_t row) {
    auto match = qdwarf::lookup(report, address);
    return match && match->table == 0 && match->row == row;
  };
  if (version >= 4) {
    check(t.rows[1].op_index == 1 && t.rows[1].address == 0x1000);
    check(t.rows[2].op_index == 1 && t.rows[2].address == 0x1002);
    check(t.rows[3].op_index == 0 && t.rows[3].address == 0x1006);
    check(t.rows[4].address == 0x100a);
    check(matches_row(0x1000, 1));
    check(matches_row(0x1001, 1));
    check(matches_row(0x1002, 2));
    check(!qdwarf::lookup(report, 0x100a));
    check(!qdwarf::lookup(report, 0x1fff));
    check(matches_row(0x2000, 5));
    check(!qdwarf::lookup(report, 0x2002));
  } else {
    check(t.rows[1].op_index == 0 && t.rows[1].address == 0x1002);
    check(matches_row(0x1000, 0));
  }
}

void positive() {
  auto opts = line_options();
  auto ss = legacy_sections();
  const std::pair<ObjectFormat, Bytes> binaries[]{
      {ObjectFormat::Elf, elf(ss)}, {ObjectFormat::Pe, windows(ss, true)},
      {ObjectFormat::Coff, windows(ss, false)}, {ObjectFormat::MachO, macho(ss)}};
  for (const auto& [format, input] : binaries) {
    auto r = qdwarf::inspect(input, opts);
    check(r.format == format);
    check(r.unit_sections.size() == 1 && r.unit_sections[0].units.size() == 1);
    check(r.unit_sections[0].units[0].header_size == 11);
    check(r.unit_sections[0].units[0].abbrev_offset == 0x11223344);
    check(!r.unit_sections[0].has_relocation_records);
    check(r.line_section == (format == ObjectFormat::MachO ? "__debug_line" : ".debug_line"));
    line_result(r, 4);
  }
  for (unsigned v : {2u, 3u}) {
    auto r = qdwarf::inspect(elf(legacy_sections(v)), opts);
    check(r.unit_sections[0].units[0].version == v);
    line_result(r, v);
  }
  Writer units{{}, true};
  for (unsigned kind = 1; kind <= 6; ++kind) units.append(unit(5, true, true, kind));
  auto r = qdwarf::inspect(elf({{".debug_info", units.data},
                              {".debug_types", unit(4, true, true, 2, true)},
                              {".debug_line", line(5, program(true, true), true, true)},
                              {".debug_line_str", {'s', 'r', 'c', 0}},
                              {".debug_str", {'a', '.', 'c', 0}}}, true), opts);
  line_result(r, 5);
  check(r.line_tables[0].offset_size == 8);
  check(r.unit_sections.size() == 2);
  auto& info = r.unit_sections[0].units;
  check(info.size() == 6);
  check(info[0].offset == 0 && info[1].offset == 24);
  check(info[1].abbrev_offset == 0x1122334455667788);
  check(info[1].type_signature == 0x1122334455667788 && info[1].type_offset == 40);
  check(info[2].kind == qdwarf::UnitKind::Partial);
  check(info[3].dwo_id == 0x1122334455667788 && info[4].dwo_id == info[3].dwo_id);
  check(info[5].kind == qdwarf::UnitKind::SplitType);
  check(r.unit_sections[1].units[0].kind == qdwarf::UnitKind::Type);
  auto image = line(4, program(false));
  auto first_size = image.size();
  auto second = line(3, program(false));
  image.insert(image.end(), second.begin(), second.end());
  auto multiple = qdwarf::inspect(elf({{".debug_line", image}}), opts);
  check(multiple.line_tables.size() == 2 && multiple.line_tables[1].offset == first_size);
  // Both tables cover this address; the first one wins.
  auto overlap = qdwarf::lookup(multiple, 0x1000);
  check(overlap && overlap->table == 0 && overlap->row == 1);
  // A caller can inspect good requested metadata without decoding an unselected section.
  check(qdwarf::inspect(elf({{".debug_info", unit(4)}, {".debug_line", {0}}})).unit_sections.size() == 1);
  auto empty = qdwarf::inspect(elf({}), opts);
  check(empty.unit_sections.empty() && empty.line_tables.empty() && !qdwarf::lookup(empty, 0));
}

void rejects(const Bytes& object, ErrorCode code, std::string_view needle,
             const qdwarf::Options& o = line_options(),
             std::string_view section = ".debug_line") {
  try {
    qdwarf::inspect(object, o);
  } catch (const qdwarf::Error& e) {
    if (e.code() != code || std::string_view(e.what()).find(needle) == std::string_view::npos
        || e.section() != section || e.offset().has_value() != !section.empty()) {
      std::cerr << "Expected [" << qdwarf::name(code) << ", " << needle << ", " << section
                << "], got: " << e.what() << '\n';
      std::exit(1);
    }
    return;
  }
  std::cerr << "Expected rejection: " << needle << '\n';
  std::exit(1);
}

void negative() {
  using enum ErrorCode;
  auto with_line = [](Bytes bytes) { return elf({{".debug_line", std::move(bytes)}}); };
  rejects({0xca, 0xfe, 0xba, 0xbe}, Unsupported, "extract", {}, "");
  rejects({0x7f, 'E', 'L', 'F'}, Malformed, "qBFD", {}, "");
  rejects({0x42, 0x42}, Unsupported, "expected ELF", {}, "");
  rejects(with_line({0, 0, 0, 0}), Malformed, "zero DWARF unit length");
  rejects(with_line({0xf1, 0xff, 0xff, 0xff}), Unsupported, "reserved DWARF");
  rejects(with_line({5, 0, 0, 0, 4, 0}), Malformed, "truncated");
  rejects(with_line(line(6, {})), Unsupported, "version 6");
  rejects(with_line(line(5, {}, false, false, 4, 1)), Unsupported, "segmented");
  rejects(with_line(line(5, {}, false, false, 3)), Unsupported, "address size 3");
  rejects(with_line(line(5, {}, false, false, 4, 0, 0x1a)), Unsupported, "line-table form");
  rejects(with_line(line(5, {})), Malformed, "missing .debug_line_str");
  rejects(elf({{".debug_line", line(5, {}, false, false, 4, 0, 0x1f, 8)},
               {".debug_line_str", {0}}}), Malformed, "outside its section");
  rejects(elf({{".debug_line", line(5, {})}, {".debug_line_str", {'a'}}}),
          Malformed, "unterminated string", line_options(), ".debug_line_str");
  auto truncated = line(4, {});
  truncated[6] = 255; // prologue length exceeds this record
  rejects(with_line(truncated), Malformed, "truncated");
  rejects(with_line(line(4, {1})), Malformed, "missing DW_LNE_end_sequence");
  rejects(with_line(line(4, {0, 0})), Malformed, "truncated byte");
  rejects(with_line(line(4, {0, 2, 1, 0})), Malformed, "unexpected extended opcode payload");
  rejects(with_line(line(4, {0, 1, 9})), Unsupported, "extended line opcode 9");
  rejects(with_line(line(4, {4, 9, 1})), Malformed, "file index");
  rejects(with_line(line(4, {2, 0x80})), Malformed, "truncated byte");
  rejects(with_line(line(4, {2, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 2})),
          Malformed, "ULEB128 overflow");
  rejects(with_line(line(4, {3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1})),
          Malformed, "SLEB128 overflow");
  rejects(with_line(line(4, {3, 0x7e})), Malformed, "line number underflow");
  Writer ops{{}};
  set_address(ops, 0xffffffff);
  ops.bytes({2, 2});
  rejects(with_line(line(4, ops.data)), Malformed, "address overflow");
  ops.data.clear();
  set_address(ops, 0x1000);
  ops.bytes({1});
  set_address(ops, 0xfff);
  ops.bytes({0, 1, 1});
  rejects(with_line(line(4, ops.data)), Malformed, "decrease");
  const auto complete = line(4, program(false));
  for (std::size_t n = 1; n < complete.size(); ++n)
    rejects(with_line(Bytes(complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(n))),
            Malformed, "truncated");
  rejects(elf({{".debug_line", line(4, {})}, {".debug_line", line(4, {})}}),
          Unsupported, "multiple sections");
  rejects(elf({{".debug_line", {}, 1, 0x800}}), Unsupported, "compressed");
  rejects(elf({{".zdebug_line", {}}}), Unsupported, "compressed", line_options(), ".zdebug_line");
  rejects(elf({{".debug_line.dwo", {}}}), Unsupported, "split", line_options(), ".debug_line.dwo");
  rejects(elf({{".debug_info", unit(5, false, false, 42)}}),
          Unsupported, "unit type 42", {}, ".debug_info");
  rejects(elf({{".debug_types", unit(3)}}), Unsupported, "only in DWARF 4", {}, ".debug_types");

  const Bytes sym(24), rela(24);
  const auto relocated = elf({{".debug_info", unit(4)},
                              {".debug_line", line(4, {})},
                              {".strtab", {0}, 3},
                              {".symtab", sym, 2, 0, 3, 1, 24},
                              {".rela.debug_info", rela, 4, 0, 4, 1, 24},
                              {".rela.debug_line", rela, 4, 0, 4, 2, 24}});
  check(qdwarf::inspect(relocated).unit_sections[0].has_relocation_records);
  rejects(relocated, Unsupported, "relocation records in line data");
  auto limit = line_options();
  const auto valid = elf(legacy_sections());
  limit.limits.max_object_bytes = 0;
  rejects(valid, LimitExceeded, "object byte", limit, "");
  limit = line_options();
  limit.units = false;
  limit.limits.max_section_bytes = 0;
  rejects(valid, LimitExceeded, "section byte", limit);
  limit = line_options();
  limit.limits.max_units = 0;
  rejects(valid, LimitExceeded, "DWARF unit", limit, ".debug_info");
  limit = line_options();
  limit.limits.max_line_tables = 0;
  rejects(valid, LimitExceeded, "line table", limit);
  limit = line_options();
  limit.limits.max_line_entries = 2;
  rejects(valid, LimitExceeded, "line entry", limit);
  limit = line_options();
  limit.limits.max_line_operations = 1;
  rejects(valid, LimitExceeded, "line operation", limit);
  limit = line_options();
  limit.limits.max_line_rows = 6;
  rejects(valid, LimitExceeded, "line row", limit);
  limit = line_options();
  limit.limits.max_string_bytes = 2;
  rejects(valid, LimitExceeded, "string length", limit);
  limit = line_options();
  limit.limits.max_string_work = 4;
  rejects(valid, LimitExceeded, "string work", limit);
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
    auto bytes = elf(legacy_sections());
    std::ofstream f(argv[2], std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    return f ? 0 : 1;
  }
  check(argc == 1);
  positive();
  negative();
}
