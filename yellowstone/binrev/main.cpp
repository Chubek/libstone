#include <binrev/BinRev.hpp>

#include "cli.hpp"
#include "options.hpp"

#include <charconv>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
constexpr uint64_t maxDisplayBytes = 4096;

// from_chars avoids accepting a leading sign, whitespace or implicit octal.
std::optional<uint64_t> number(std::string_view text) {
  const bool hex = text.starts_with("0x") || text.starts_with("0X");
  if (hex) text.remove_prefix(2);
  if (text.empty()) return std::nullopt;
  uint64_t value = 0;
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, value, hex ? 16 : 10);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return value;
}

// All text derived from the object is treated as data, not terminal control
// sequences. Quote names so empty fields, spaces and delimiters are visible.
std::string quoteText(std::string_view value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') result += '\\';
    if (qobj::isPrintable(c))
      result += char(c);
    else
      result += "\\x" + qobj::hex(c, 2);
  }
  return result + '"';
}

int error(std::string_view message, int status) {
  std::cerr << "binrev: " << quoteText(message) << '\n';
  return status;
}

void printSymbols(const std::vector<qbfd::Symbol>& table, bool dynamic) {
  for (const auto& s : table) {
    std::cout << (dynamic ? "dynamic" : "static") << '[' << s.index << "] " << quoteText(s.name)
              << " type=" << qobj::symbolType(s) << " value=0x" << qobj::hex(s.value)
              << " size=0x" << qobj::hex(s.size) << " flags=0x" << qobj::hex(s.flags);
    if (s.section) std::cout << " section=" << *s.section;
    std::cout << '\n';
  }
}

int printRelocations(const qbfd::ObjectFile& file) {
  std::cout << "Relocations:\n";
  for (const auto& section : file.sections()) {
    auto relocs = file.relocations(section);
    if (!relocs) return error(relocs.error().message, 1);
    for (const auto& r : *relocs) {
      std::cout << "section[" << section.index << "]+0x" << qobj::hex(r.offset)
                << ' ' << quoteText(r.typeName) << " native-type=" << r.nativeType;
      if (r.symbol) {
        const auto& symbols = r.dynamicSymbol ? file.dynamicSymbols() : file.symbols();
        std::cout << ' ' << (r.dynamicSymbol ? "dynamic" : "static") << '[' << *r.symbol << ']';
        if (*r.symbol < symbols.size() && symbols[*r.symbol].index == *r.symbol)
          std::cout << ' ' << quoteText(symbols[*r.symbol].name);
      }
      if (r.targetSection) std::cout << " target-section=" << *r.targetSection;
      if (r.bitSize) std::cout << " bits=" << unsigned(r.bitSize);
      if (r.pcRelative) std::cout << " pc-relative";
      if (r.hasAddend) std::cout << " addend=" << r.addend;
      if (r.scattered) std::cout << " scattered=0x" << qobj::hex(r.nativeValue);
      std::cout << '\n';
    }
  }
  return 0;
}

int run(int argc, char** argv) {
  qobj::Options options("binrev", "FILE", {
      {'h', "help", false, {}, "Show this help"},
      {'S', "sections", false, {}, "List canonical sections"},
      {'s', "symbols", false, {}, "List static and dynamic symbols"},
      {'r', "relocations", false, {}, "List relocations by owning section"},
      {0, "address", true, "VA", "Locate a preferred virtual address in a linked image"},
      {0, "offset", true, "OFF", "Locate a raw file offset"},
      {0, "section", true, "INDEX:OFF", "Locate a section index and section-relative offset"},
      {0, "bytes", true, "COUNT", "Read COUNT bytes at the selected location (1-4096)"},
  });
  if (!options.parse(argc, argv)) return error(options.error(), 2);
  if (options.has("help")) {
    std::cout << options.usage();
    return 0;
  }
  if (options.positional().size() != 1) return error("expected one input file; see --help", 2);

  const auto positions = options.valueList("address").size() + options.valueList("offset").size() +
                         options.valueList("section").size();
  if (positions > 1) return error("specify one of --address, --offset or --section once", 2);
  std::optional<binrev::Position> position;
  if (options.has("address")) {
    auto value = number(options.value("address"));
    if (!value) return error("invalid virtual address", 2);
    position = binrev::Address{*value};
  } else if (options.has("offset")) {
    auto value = number(options.value("offset"));
    if (!value) return error("invalid file offset", 2);
    position = binrev::FileOffset{*value};
  } else if (options.has("section")) {
    std::string_view value = options.value("section");
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) return error("expected INDEX:OFF for --section", 2);
    auto index = number(value.substr(0, colon));
    auto offset = number(value.substr(colon + 1));
    if (!index || *index > std::numeric_limits<uint32_t>::max() || !offset)
      return error("invalid section index or offset", 2);
    position = binrev::SectionOffset{static_cast<uint32_t>(*index), *offset};
  }
  std::optional<uint64_t> length;
  if (options.has("bytes")) {
    if (!position) return error("--bytes requires a location", 2);
    length = number(options.value("bytes"));
    if (!length || !*length || *length > maxDisplayBytes)
      return error("--bytes requires a count between 1 and 4096", 2);
  }
  auto binary = binrev::Binary::open(options.positional().front());
  if (!binary) return error(binary.error().message, 1);
  const auto& file = binary->object();
  std::optional<binrev::Location> location;
  std::span<const uint8_t> bytes;
  if (position) {
    auto loc = binary->locate(*position);
    if (!loc) return error(loc.error().message, 1);
    location = std::move(*loc);
    if (length) {
      auto result = binary->readBytes(*position, *length);
      if (!result) return error(result.error().message, 1);
      bytes = *result;
    }
  }
  std::cout << qobj::formatName(file.format()) << ' ' << file.targetName() << ' '
            << qbfd::toString(file.arch()) << ' ' << (file.is64Bit() ? "64-bit" : "32-bit")
            << ' ' << qbfd::toString(file.endian()) << "-endian " << qbfd::toString(file.fileType())
            << " entry=0x" << qobj::hex(file.entryPoint())
            << " image-base=0x" << qobj::hex(file.imageBase()) << '\n';
  if (options.has("sections")) {
    std::cout << "Sections:\n";
    for (const auto& s : file.sections())
      std::cout << "section[" << s.index << "] " << quoteText(s.name)
                << " vma=0x" << qobj::hex(s.vma) << " size=0x" << qobj::hex(s.size)
                << " file-offset=0x" << qobj::hex(s.fileOffset)
                << " file-size=0x" << qobj::hex(s.fileSize) << " flags=" << qobj::sectionFlags(s) << '\n';
  }
  if (options.has("symbols")) {
    std::cout << "Symbols:\n";
    printSymbols(file.symbols(), false);
    printSymbols(file.dynamicSymbols(), true);
  }
  if (options.has("relocations")) {
    if (const auto status = printRelocations(file)) return status;
  }
  if (location) {
    std::cout << "Location:";
    if (location->section)
      std::cout << " section[" << location->section->index << "]+0x"
                << qobj::hex(location->section->offset);
    if (location->address) std::cout << " address=0x" << qobj::hex(location->address->value);
    if (location->fileOffset)
      std::cout << " file-offset=0x" << qobj::hex(location->fileOffset->value);
    else
      std::cout << " no file backing";
    if (location->symbol) {
      const auto& match = *location->symbol;
      std::cout << " symbol=" << quoteText(match.symbol.name) << "+0x" << qobj::hex(match.displacement)
                << (match.dynamic ? " (dynamic)" : " (static)");
    }
    std::cout << '\n';
  }
  for (size_t i = 0; i < bytes.size(); ++i)
    std::cout << (i % 16 ? " " : "\n  ") << qobj::hex(bytes[i], 2);
  if (!bytes.empty()) std::cout << '\n';
  return std::cout ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    return error(e.what(), 1);
  }
}
