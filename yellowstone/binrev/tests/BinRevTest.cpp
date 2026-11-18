#include <binrev/BinRev.hpp>

#include "Fixtures.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>

namespace {
using namespace binrev;
using namespace std::string_view_literals;
using Code = qbfd::Error::Code;

#define CHECK(...)                                                               \
  do {                                                                           \
    if (!(__VA_ARGS__)) {                                                        \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n';      \
      std::exit(1);                                                              \
    }                                                                            \
  } while (false)

Binary open(fixture::Bytes bytes, qbfd::Format format) {
  auto file = Binary::open(std::move(bytes), "fixture");
  if (!file) std::cerr << file.error().message << '\n';
  CHECK(file && file->object().format() == format);
  return std::move(*file);
}

template <class T>
void fails(const qbfd::Expected<T>& result, Code code) {
  if (result) std::cerr << "unexpected success\n";
  else if (result.error().code != code) std::cerr << result.error().message << '\n';
  CHECK(!result && result.error().code == code);
}

void elf() {
  constexpr uint64_t va = 0x401000;
  auto file = open(fixture::elf(), qbfd::Format::ELF);
  CHECK(file.object().sections().size() == 7);
  CHECK(file.object().sections()[1].name == ".text");
  auto at = file.locate(Address{va + 1});
  CHECK(at && at->address->value == va + 1 && at->fileOffset->value == 0x101);
  CHECK(at->section->index == 1 && at->section->offset == 1);
  CHECK(at->symbol && at->symbol->symbol.name == "region" && at->symbol->displacement == 1);
  CHECK(!at->symbol->dynamic);
  auto fromOffset = file.locate(FileOffset{0x104});
  CHECK(fromOffset && fromOffset->address->value == va + 4);
  CHECK(fromOffset->symbol && fromOffset->symbol->symbol.name == "marker");
  auto pastSized = file.locate(Address{va + 3});
  auto pastUnsized = file.locate(Address{va + 5});
  CHECK(pastSized && !pastSized->symbol && pastUnsized && !pastUnsized->symbol);
  auto bss = file.locate(Address{va + 9});
  CHECK(bss && bss->section->index == 2 && !bss->fileOffset);
  CHECK(bss->symbol && bss->symbol->symbol.name == "zeroes");
  auto bytes = file.readBytes(Address{va + 6}, 2);
  CHECK(bytes && bytes->size() == 2 && (*bytes)[0] == '2' && (*bytes)[1] == '3');
  auto header = file.locate(FileOffset{0});
  CHECK(header && header->fileOffset && !header->section && !header->address);
  auto strtab = file.locate(SectionOffset{4, 1});
  CHECK(strtab && strtab->fileOffset->value == 0x191 && !strtab->address);

  fails(file.readBytes(Address{va + 6}, 3), Code::OutOfRange);
  fails(file.readBytes(Address{va + 8}, 1), Code::OutOfRange);
  fails(file.readBytes(Address{va}, 0), Code::OutOfRange);
  fails(file.locate(SectionOffset{1, 8}), Code::OutOfRange);
  fails(file.readBytes(SectionOffset{UINT32_MAX, 0}, 1), Code::OutOfRange);
  fails(file.locate(Address{va - 1}), Code::OutOfRange);
  fails(file.readBytes(Address{va}, UINT64_MAX), Code::OutOfRange);
  fails(file.readBytes(FileOffset{UINT64_MAX}, 8), Code::OutOfRange);
  fails(file.readBytes(FileOffset{1}, UINT64_MAX), Code::OutOfRange);
  fails(file.readBytes(FileOffset{0x43f}, 2), Code::OutOfRange);
  fails(file.locate(FileOffset{0x440}), Code::OutOfRange);
  CHECK(file.readBytes(FileOffset{0x43f}, 1));

  auto relocs = file.object().relocations(file.object().sections()[1]);
  CHECK(relocs && relocs->size() == 1);
  const auto& reloc = relocs->front();
  CHECK(reloc.offset == 0 && reloc.symbol == 2 && reloc.typeName == "R_X86_64_PC32");
  CHECK(reloc.pcRelative && reloc.hasAddend && reloc.addend == -4);
  CHECK(file.object().symbols()[*reloc.symbol].name == "marker");

  auto obj = open(fixture::elf(true), qbfd::Format::ELF);
  auto loc = obj.locate(SectionOffset{1, 1});
  CHECK(loc && !loc->address && loc->fileOffset->value == 0x101);
  CHECK(loc->symbol && loc->symbol->symbol.name == "region");
  fails(obj.locate(Address{0x101}), Code::Unsupported);
  // Moving the owner preserves a previously borrowed byte span.
  auto data = obj.readBytes(SectionOffset{1, 0}, 5);
  auto owner = std::move(obj);
  CHECK(data && (*data)[0] == 'A' && (*data)[4] == 'T');
  CHECK(owner.object().fileType() == qbfd::FileType::Relocatable);
}

void pe() {
  constexpr uint64_t va = 0x140001000;
  auto file = open(fixture::pe(), qbfd::Format::PE);
  CHECK(file.object().entryPoint() == va);
  auto at = file.locate(Address{va});
  CHECK(at && at->fileOffset->value == 0x200 && at->section->index == 0);
  CHECK(at->symbol && at->symbol->dynamic && at->symbol->symbol.name == "exported");
  auto next = file.locate(Address{va + 1});
  CHECK(next && !next->symbol);
  auto tail = file.locate(Address{va + 0x10});
  CHECK(tail && !tail->fileOffset && tail->section->offset == 0x10);
  auto last = file.readBytes(Address{va + 0xf}, 1);
  CHECK(last && (*last)[0] == 'b');
  fails(file.readBytes(Address{va + 0xf}, 2), Code::OutOfRange);
  fails(file.readBytes(SectionOffset{0, 0x10}, 1), Code::OutOfRange);
  fails(file.locate(Address{0x1000}), Code::OutOfRange); // RVA, not a VA
  fails(file.locate(Address{0x140000000}), Code::OutOfRange); // Image headers
  auto relocs = file.object().relocations(file.object().sections()[0]);
  CHECK(relocs && relocs->size() == 1);
  CHECK(relocs->front().offset == 8 && relocs->front().bitSize == 64 &&
        !relocs->front().symbol);

  auto padded = fixture::pe();
  fixture::put(padded, 0xf8 + 8, 0xc, 4); // VirtualSize < SizeOfRawData
  auto padFile = open(std::move(padded), qbfd::Format::PE);
  auto padding = padFile.locate(FileOffset{0x20c});
  CHECK(padding && !padding->section && !padding->address);
  CHECK(padFile.readBytes(FileOffset{0x20c}, 4));
}

void machO() {
  constexpr uint64_t va = 0x100000140;
  auto file = open(fixture::machO(), qbfd::Format::MachO);
  auto loc = file.locate(Address{va});
  CHECK(loc && loc->section->index == 0 && loc->fileOffset->value == 0x140);
  CHECK(loc->symbol && loc->symbol->symbol.name == "_marker" && !loc->symbol->dynamic);
  auto bss = file.locate(SectionOffset{1, 4});
  CHECK(bss && !bss->fileOffset && bss->address->value == va + 0x24);
  fails(file.readBytes(SectionOffset{1, 4}, 1), Code::OutOfRange);
  auto relocs = file.object().relocations(file.object().sections()[0]);
  CHECK(relocs && relocs->size() == 2);
  CHECK((*relocs)[0].symbol == 1 && (*relocs)[0].typeName == "MACHO_RELOC_0");
  CHECK((*relocs)[1].targetSection == 1 && (*relocs)[1].offset == 4);
  auto obj = open(fixture::machO(true), qbfd::Format::MachO);
  auto objLoc = obj.locate(FileOffset{0x140});
  CHECK(objLoc && objLoc->symbol && objLoc->symbol->symbol.name == "_marker");
  CHECK(!objLoc->address);
  fails(obj.locate(Address{0x20}), Code::Unsupported);
}

void malformedAndOverlap() {
  auto truncated = fixture::elf();
  truncated.pop_back();
  fails(Binary::open(std::move(truncated)), Code::Malformed);
  fails(Binary::open(fixture::Bytes{}), Code::UnknownFormat);
  fails(Binary::open(fixture::fatMachO()), Code::Unsupported);

  auto overlap = fixture::elf();
  fixture::put(overlap, 0x280 + 2 * 64 + 16, 0x401004, 8); // BSS overlaps .text
  auto file = open(std::move(overlap), qbfd::Format::ELF);
  fails(file.locate(Address{0x401004}), Code::Ambiguous);
  fails(file.readBytes(Address{0x401002}, 3), Code::Ambiguous);
  CHECK(file.readBytes(Address{0x401002}, 2));
  CHECK(file.readBytes(SectionOffset{1, 2}, 3)); // Explicit section disambiguates.

  overlap = fixture::elf();
  fixture::put(overlap, 0x280 + 4 * 64 + 24, 0xf8, 8); // strtab aliasing .text
  // Keep the aliased string table valid (the fixture resolves names at 1/8/15).
  fixture::text(overlap, 0xf8, "\0region\0marker\0zeroes\0"sv);
  auto aliased = open(std::move(overlap), qbfd::Format::ELF);
  fails(aliased.locate(FileOffset{0x100}), Code::Ambiguous);
  CHECK(aliased.readBytes(FileOffset{0x100}, 1)); // Raw byte reads don't need a section.

  auto edge = fixture::machO();
  fixture::put(edge, 104 + 32, UINT64_MAX - 1, 8);
  auto top = open(std::move(edge), qbfd::Format::MachO);
  CHECK(top.readBytes(Address{UINT64_MAX}, 1));
  fails(top.readBytes(Address{UINT64_MAX}, 2), Code::OutOfRange);
  fails(top.locate(SectionOffset{0, 2}), Code::OutOfRange);

  auto edge32 = fixture::pe(false);
  fixture::put(edge32, 0x58 + 28, UINT32_MAX - 0x1001, 4);
  auto top32 = open(std::move(edge32), qbfd::Format::PE);
  CHECK(!top32.object().is64Bit() && top32.object().entryPoint() == UINT32_MAX - 1);
  CHECK(top32.readBytes(Address{UINT32_MAX}, 1));
  fails(top32.readBytes(Address{UINT32_MAX}, 2), Code::OutOfRange);
  fails(top32.locate(SectionOffset{0, 2}), Code::OutOfRange);
}

void write(const std::filesystem::path& name, const fixture::Bytes& bytes) {
  std::ofstream out(name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  CHECK(out);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    write(dir / "test.elf", fixture::elf());
    write(dir / "test.o", fixture::elf(true));
    write(dir / "test.exe", fixture::pe());
    write(dir / "test.macho", fixture::machO());
    write(dir / "fat.macho", fixture::fatMachO());
    auto escaped = fixture::machO();
    escaped[0x179] = 0x1b;
    write(dir / "escaped.macho", escaped);
    write(dir / "oversize.elf", {});
    std::filesystem::resize_file(dir / "oversize.elf", maxInputBytes + 1);
    return 0;
  }
  CHECK(argc == 1);
  elf();
  pe();
  machO();
  malformedAndOverlap();
}
