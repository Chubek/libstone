#include <qdwarf/qdwarf.hpp>

#include <charconv>
#include <iomanip>
#include <iostream>
#include <system_error>

namespace {

void help(std::ostream& out) {
  out << "Usage: qdwarf [--units] [--lines] [--address ADDRESS] [--] OBJECT\n"
         "Inspect DWARF metadata in ELF, PE/COFF, or thin Mach-O.\n"
         "  --units            List unit headers (default without other options)\n"
         "  --lines            List line tables and decoded rows\n"
         "  --address ADDRESS  Find the source row at a linked address (decimal or 0x hex)\n"
         "  -h, --help          Show this help\n";
}

// Do not send terminal controls or literal line breaks from object metadata.
void escaped(std::ostream& out, std::string_view text, bool quote = false) {
  constexpr char hex[] = "0123456789abcdef";
  if (quote) out << '"';
  for (unsigned char ch : text) {
    if (ch < 32 || ch > 126)
      out << "\\x" << hex[ch >> 4] << hex[ch & 15];
    else {
      if (quote && (ch == '"' || ch == '\\')) out << '\\';
      out << static_cast<char>(ch);
    }
  }
  if (quote) out << '"';
}

std::optional<std::uint64_t> address(std::string_view arg) {
  int base = 10;
  if (arg.starts_with("0x") || arg.starts_with("0X")) {
    arg.remove_prefix(2);
    base = 16;
  }
  if (arg.empty()) return {};
  std::uint64_t value;
  auto end = arg.data() + arg.size();
  const auto result = std::from_chars(arg.data(), end, value, base);
  if (result.ec != std::errc{} || result.ptr != end) return {};
  return value;
}

void show_units(const qdwarf::Report& report) {
  if (report.unit_sections.empty()) std::cout << "No DWARF unit sections.\n";
  for (const auto& section : report.unit_sections) {
    std::cout << section.name;
    if (section.has_relocation_records) std::cout << " (relocation records present; offsets are raw)";
    std::cout << ": " << section.units.size() << " unit(s)\n";
    for (const auto& unit : section.units) {
      std::cout << "  +0x" << std::hex << unit.offset
                << " length=0x" << unit.unit_length
                << " abbrev=0x" << unit.abbrev_offset << std::dec
                << " DWARF" << unsigned(unit.offset_size) * 8
                << " v" << unit.version << ' ' << qdwarf::name(unit.kind)
                << " addr-size=" << unsigned(unit.address_size);
      if (unit.type_signature) std::cout << " signature=0x" << std::hex << *unit.type_signature;
      if (unit.type_offset) std::cout << " type-offset=0x" << std::hex << *unit.type_offset;
      if (unit.dwo_id) std::cout << " dwo-id=0x" << std::hex << *unit.dwo_id;
      std::cout << std::dec << '\n';
    }
  }
}

void show_source(const qdwarf::LineTable& table, const qdwarf::LineRow& row) {
  const auto* file = table.file(row.file);
  if (!file) return;
  const auto* dir = table.directory(*file);
  if (dir) {
    std::cout << " directory=";
    escaped(std::cout, *dir, true);
  }
  std::cout << " path=";
  escaped(std::cout, file->path, true);
  std::cout << ':' << row.line << ':' << row.column;
}

void show_lines(const qdwarf::Report& report) {
  if (report.line_section.empty()) std::cout << "No DWARF line section.\n";
  else std::cout << report.line_section << ": " << report.line_tables.size() << " table(s)\n";
  for (const auto& table : report.line_tables) {
    std::cout << "  +0x" << std::hex << table.offset << " length=0x" << table.unit_length
              << std::dec << " DWARF" << unsigned(table.offset_size) * 8
              << " v" << table.version << " addr-size=" << unsigned(table.address_size)
              << " files=" << table.files.size() << " rows=" << table.rows.size() << '\n';
    for (const auto& row : table.rows) {
      std::cout << "    0x" << std::hex << row.address << std::dec
                << " op-index=" << unsigned(row.op_index);
      if (row.end_sequence) {
        std::cout << " end_sequence\n";
        continue;
      }
      std::cout << " file=" << row.file;
      show_source(table, row);
      if (row.is_stmt) std::cout << " is_stmt";
      if (row.basic_block) std::cout << " basic_block";
      if (row.prologue_end) std::cout << " prologue_end";
      if (row.epilogue_begin) std::cout << " epilogue_begin";
      if (row.isa) std::cout << " isa=" << row.isa;
      if (row.discriminator) std::cout << " discriminator=" << row.discriminator;
      std::cout << '\n';
    }
  }
}

int usage_error(std::string_view message, std::string_view value = {}) {
  std::cerr << "qdwarf: " << message;
  if (!value.empty()) {
    std::cerr << ' ';
    escaped(std::cerr, value, true);
  }
  std::cerr << '\n';
  help(std::cerr);
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  qdwarf::Options options;
  options.units = false;
  bool show_rows = false, end_options = false;
  std::optional<std::uint64_t> query;
  const char* input = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (!end_options && (arg == "--help" || arg == "-h")) {
      help(std::cout);
      return 0;
    }
    if (!end_options && arg == "--") end_options = true;
    else if (!end_options && arg == "--units") options.units = true;
    else if (!end_options && arg == "--lines") show_rows = true;
    else if (!end_options && arg == "--address") {
      if (query || i + 1 == argc) return usage_error("expected one address after --address");
      query = address(argv[++i]);
      if (!query) return usage_error("invalid address", argv[i]);
    } else {
      if (!end_options && arg.starts_with('-')) return usage_error("unknown option", arg);
      if (input) return usage_error("expected exactly one input file");
      input = argv[i];
    }
  }
  if (!input) return usage_error("missing input file");
  options.lines = show_rows || query.has_value();
  if (!options.lines) options.units = true;
  try {
    const auto report = qdwarf::inspect_file(input, options);
    std::cout << qdwarf::name(report.format) << '\n';
    if (options.units) show_units(report);
    if (show_rows) show_lines(report);
    if (query) {
      auto match = qdwarf::lookup(report, *query);
      std::cout << "0x" << std::hex << *query << std::dec;
      if (!match) {
        std::cout << ": no source row\n";
        return 1;
      }
      const auto& table = report.line_tables[match->table];
      std::cout << ':';
      show_source(table, table.rows[match->row]);
      std::cout << '\n';
    }
  } catch (const std::exception& e) {
    std::cerr << "qdwarf: ";
    escaped(std::cerr, input, true);
    std::cerr << ": ";
    escaped(std::cerr, e.what());
    std::cerr << '\n';
    return 1;
  }
  return 0;
}
