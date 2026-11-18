#include <binrev/BinRev.hpp>

#include <qBFD.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace binrev {
namespace {
using qbfd::Error;
using qbfd::Expected;
using qbfd::ObjectFile;
using qbfd::Section;
using Bytes = std::span<const uint8_t>;

Error rangeError(const char* message) {
  return {Error::Code::OutOfRange, message};
}

bool contains(uint64_t start, uint64_t size, uint64_t position) {
  return position >= start && position - start < size;
}

bool isRelocatable(const ObjectFile& object) {
  return object.fileType() == qbfd::FileType::Relocatable;
}

uint64_t addressLimit(const ObjectFile& object) {
  return object.is64Bit() ? UINT64_MAX : UINT32_MAX;
}

Expected<const Section*> sectionForAddress(const ObjectFile& object,
                                           Address address, uint64_t length) {
  if (isRelocatable(object))
    return Error{Error::Code::Unsupported,
                 "relocatable input has no linked addresses; use a section offset"};
  const auto limit = addressLimit(object);
  if (address.value > limit || length - 1 > limit - address.value)
    return rangeError("virtual address range exceeds the image address width");

  const Section* owner = nullptr;
  uint64_t nextSection = std::numeric_limits<uint64_t>::max();
  const Error overlap{Error::Code::Ambiguous, "address range intersects overlapping sections"};
  for (const auto& section : object.sections()) {
    if (!(section.flags & qbfd::sec::Alloc) || !section.size) continue;
    if (contains(section.vma, section.size, address.value)) {
      if (owner) return overlap;
      owner = &section;
    } else if (section.vma > address.value) {
      nextSection = std::min(nextSection, section.vma - address.value);
    }
  }
  if (!owner) return rangeError("address is not in an allocated section");
  const auto remaining = owner->size - (address.value - owner->vma);
  if (nextSection < std::min(length, remaining)) return overlap;
  if (length > remaining) return rangeError("address range exceeds the section");
  return owner;
}

Expected<const Section*> sectionByIndex(const ObjectFile& object, uint32_t index) {
  const auto& sections = object.sections();
  const auto it = std::find_if(sections.begin(), sections.end(),
                               [index](const Section& s) { return s.index == index; });
  if (it == sections.end()) return rangeError("unknown section index");
  return &*it;
}

// Use only the file-backed portion of the canonical section, never raw PE
// padding or an inferred file offset in a virtual-only tail.
uint64_t backedSize(const Section& section) {
  if (section.flags & qbfd::sec::Bss) return 0;
  return std::min(section.size, section.fileSize);
}

std::optional<SymbolMatch> attribute(const ObjectFile& object,
                                    const Section& section, uint64_t offset) {
  const qbfd::Symbol* best = nullptr;
  bool dynamic = false;
  uint64_t bestDisplacement = 0;
  // In qBFD, ELF ET_REL values are section-relative. Other section-defined
  // symbols use the same coordinate space as their section's declared VMA.
  const bool relative = isRelocatable(object) && object.format() == qbfd::Format::ELF;
  const uint64_t base = relative ? 0 : section.vma;
  auto consider = [&](const qbfd::Symbol& symbol, bool isDynamic) {
    if (symbol.name.empty() || symbol.flags & (qbfd::sym::Undefined | qbfd::sym::Absolute |
                                               qbfd::sym::Imported | qbfd::sym::Debug))
      return;
    using Kind = qbfd::SymbolKind;
    if (symbol.kind == Kind::Section || symbol.kind == Kind::File ||
        symbol.kind == Kind::Common || symbol.kind == Kind::Tls)
      return;
    // PE exports emitted by qBFD have a VA but no section index. Match only
    // direct exports at that exact address, never imports or forwarders.
    const bool directPeExport =
        !symbol.section && isDynamic && object.format() == qbfd::Format::PE &&
        (symbol.flags & qbfd::sym::Exported) && symbol.kind != Kind::Indirect;
    if (!directPeExport && symbol.section != section.index) return;
    if (!contains(base, section.size, symbol.value)) return;
    const uint64_t start = symbol.value - base;
    if (offset < start) return;
    const uint64_t displacement = offset - start;
    // A missing size is not an unbounded extent. The symbol matches exactly.
    if (displacement && (directPeExport || displacement >= symbol.size)) return;
    if (!best || displacement < bestDisplacement) {
      best = &symbol;
      bestDisplacement = displacement;
      dynamic = isDynamic;
    }
  };
  for (const auto& symbol : object.symbols()) consider(symbol, false);
  for (const auto& symbol : object.dynamicSymbols()) consider(symbol, true);
  if (!best) return std::nullopt;
  return SymbolMatch{*best, dynamic, bestDisplacement};
}

Expected<Location> sectionLocation(const ObjectFile& object, const Section& s,
                                   uint64_t offset) {
  if (offset >= s.size) return rangeError("offset is outside the section");
  Location result;
  result.section = SectionOffset{s.index, offset};
  if (!isRelocatable(object) && (s.flags & qbfd::sec::Alloc)) {
    const auto limit = addressLimit(object);
    if (s.vma > limit || offset > limit - s.vma)
      return rangeError("section virtual address exceeds the image address width");
    result.address = Address{s.vma + offset};
  }
  if (offset < backedSize(s)) {
    auto bytes = object.data();
    if (s.fileOffset > bytes.size() || offset >= bytes.size() - s.fileOffset)
      return Error{Error::Code::Malformed, "section backing lies outside the file"};
    result.fileOffset = FileOffset{s.fileOffset + offset};
  }
  result.symbol = attribute(object, s, offset);
  return result;
}

Expected<Bytes> readSection(const ObjectFile& object, const Section& s,
                            uint64_t offset, uint64_t length) {
  if (const auto backed = backedSize(s); offset >= backed || length > backed - offset)
    return rangeError("byte range exceeds the section's file-backed contents");
  auto bytes = object.sectionContents(s);
  if (!bytes) return bytes.error();
  if (offset > bytes->size() || length > bytes->size() - offset)
    return Error{Error::Code::Malformed, "section contents are shorter than declared"};
  return bytes->subspan(static_cast<size_t>(offset), static_cast<size_t>(length));
}

Error inputTooLarge() {
  return rangeError("input exceeds the 256 MiB size limit");
}
}  // namespace

Binary::Binary(std::unique_ptr<ObjectFile> object) : object_(std::move(object)) {}

qbfd::Expected<Binary> Binary::open(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (status.type() == std::filesystem::file_type::not_found)
    return Error{Error::Code::FileNotFound, "file not found: " + path.string()};
  if (ec) return Error{Error::Code::IoError, "cannot stat input: " + path.string()};
  if (!std::filesystem::is_regular_file(status))
    return Error{Error::Code::Unsupported, "input must be a regular file"};

  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return Error{Error::Code::IoError, "cannot open input: " + path.string()};
  const std::streamoff size = input.tellg();
  if (size < 0) return Error{Error::Code::IoError, "cannot determine input size"};
  if (static_cast<uint64_t>(size) > maxInputBytes) return inputTooLarge();
  input.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()), size)) ||
      input.peek() != std::char_traits<char>::eof() || input.bad())
    return Error{Error::Code::IoError, "cannot read input at its stated size"};
  return open(std::move(bytes), path);
}

qbfd::Expected<Binary> Binary::open(std::vector<uint8_t> bytes,
                                   const std::filesystem::path& label) {
  if (bytes.size() > maxInputBytes) return inputTooLarge();
  if (qbfd::macho::isFat(bytes))
    return Error{Error::Code::Unsupported, "universal Mach-O: supply a separate thin slice"};
  auto opened = qbfd::open(std::move(bytes), label);
  if (!opened) return opened.error();
  return Binary(std::move(opened->object));
}

qbfd::Expected<Location> Binary::locate(Position position) const {
  const auto& file = object();
  if (const auto* address = std::get_if<Address>(&position)) {
    auto section = sectionForAddress(file, *address, 1);
    if (!section) return section.error();
    return sectionLocation(file, **section, address->value - (*section)->vma);
  }
  if (const auto* at = std::get_if<SectionOffset>(&position)) {
    auto section = sectionByIndex(file, at->index);
    if (!section) return section.error();
    return sectionLocation(file, **section, at->offset);
  }

  const auto at = std::get<FileOffset>(position);
  if (at.value >= file.data().size()) return rangeError("file offset is outside the file");
  const Section* owner = nullptr;
  for (const auto& s : file.sections()) {
    if (!contains(s.fileOffset, backedSize(s), at.value)) continue;
    if (owner) return Error{Error::Code::Ambiguous, "file offset is in overlapping sections"};
    owner = &s;
  }
  if (owner) return sectionLocation(file, *owner, at.value - owner->fileOffset);
  Location result;
  result.fileOffset = at;
  return result;
}

qbfd::Expected<Bytes> Binary::readBytes(Position position, uint64_t length) const {
  if (length == 0) return rangeError("byte length must be positive");
  const auto& file = object();
  if (const auto* address = std::get_if<Address>(&position)) {
    auto section = sectionForAddress(file, *address, length);
    if (!section) return section.error();
    return readSection(file, **section, address->value - (*section)->vma, length);
  }
  if (const auto* at = std::get_if<SectionOffset>(&position)) {
    auto section = sectionByIndex(file, at->index);
    if (!section) return section.error();
    return readSection(file, **section, at->offset, length);
  }

  const auto offset = std::get<FileOffset>(position).value;
  const auto bytes = file.data();
  if (offset >= bytes.size() || length > bytes.size() - offset)
    return rangeError("byte range exceeds file contents");
  return bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(length));
}

}  // namespace binrev
