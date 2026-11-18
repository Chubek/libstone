#pragma once

// Tiny, hand-laid-out fixtures with inert synthetic data. No host object
// layout, assembler, linker, or runtime is needed to exercise the readers.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fixture {

using Bytes = std::vector<uint8_t>;
using namespace std::string_view_literals;

inline void put(Bytes& file, size_t at, uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    file.at(at + i) = static_cast<uint8_t>(value >> (8 * i));
}

inline void text(Bytes& file, size_t at, std::string_view value) {
  for (char c : value) file.at(at++) = static_cast<uint8_t>(c);
}

inline Bytes elf(bool relocatable = false) {
  Bytes file(0x440);
  text(file, 0, "\x7f" "ELF\x02\x01\x01"sv); // ELF64 little endian
  put(file, 16, relocatable ? 1 : 2, 2);   // ET_REL or ET_EXEC
  put(file, 18, 62, 2);                    // EM_X86_64
  put(file, 20, 1, 4);
  put(file, 24, relocatable ? 0 : 0x401000, 8);
  put(file, 40, 0x280, 8); // Section headers
  put(file, 52, 64, 2);
  put(file, 58, 64, 2);
  put(file, 60, 7, 2);
  put(file, 62, 6, 2);

  text(file, 0x100, "AUDIT123"sv);
  text(file, 0x190, "\0region\0marker\0zeroes\0"sv);
  text(file, 0x1e0, "\0.text\0.bss\0.symtab\0.strtab\0.rela.text\0.shstrtab\0"sv);
  const auto dataAddress = relocatable ? 0x100u : 0x401000u;
  auto section = [&](size_t index, unsigned name, unsigned type, unsigned flags,
                     uint64_t address, uint64_t off, unsigned size,
                     unsigned link = 0, unsigned info = 0, unsigned entsize = 0) {
    size_t at = 0x280 + index * 64;
    put(file, at, name, 4);
    put(file, at + 4, type, 4);
    put(file, at + 8, flags, 8);
    put(file, at + 16, address, 8);
    put(file, at + 24, off, 8);
    put(file, at + 32, size, 8);
    put(file, at + 40, link, 4);
    put(file, at + 44, info, 4);
    put(file, at + 48, 1, 8);
    put(file, at + 56, entsize, 8);
  };
  section(1, 1, 1, 6, dataAddress, 0x100, 8);
  section(2, 7, 8, 3, dataAddress + 8, 0x108, 8);
  section(3, 12, 2, 0, 0, 0x110, 96, 4, 1, 24);
  section(4, 20, 3, 0, 0, 0x190, 22);
  section(5, 28, 4, 0, 0, 0x1c0, 24, 3, 1, 24);
  section(6, 39, 3, 0, 0, 0x1e0, 49);
  auto symbol = [&](size_t index, unsigned name, unsigned shndx,
                    unsigned off, unsigned size) {
    size_t at = 0x110 + index * 24;
    put(file, at, name, 4);
    put(file, at + 4, 0x11, 1); // STB_GLOBAL | STT_OBJECT
    put(file, at + 6, shndx, 2);
    put(file, at + 8, relocatable ? off : dataAddress + (shndx == 2 ? 8 : 0) + off, 8);
    put(file, at + 16, size, 8);
  };
  symbol(1, 1, 1, 0, 3);
  symbol(2, 8, 1, 4, 0);
  symbol(3, 15, 2, 0, 4);
  put(file, 0x1c0, relocatable ? 0 : dataAddress, 8);
  put(file, 0x1c8, (uint64_t{2} << 32) | 2, 8); // Symbol index 2, R_X86_64_PC32
  put(file, 0x1d0, uint64_t(-4), 8);
  return file;
}

inline Bytes pe(bool wide = true) {
  Bytes file(0x2c0);
  text(file, 0, "MZ"sv);
  put(file, 0x3c, 0x40, 4);
  text(file, 0x40, "PE\0\0"sv);
  put(file, 0x44, wide ? 0x8664 : 0x14c, 2);
  put(file, 0x46, 2, 2);
  put(file, 0x54, 160, 2);
  put(file, 0x56, 2, 2);
  constexpr size_t opt = 0x58;
  put(file, opt, wide ? 0x20b : 0x10b, 2); // PE32+ or PE32
  put(file, opt + 16, 0x1000, 4);
  if (wide) put(file, opt + 24, 0x140000000, 8);
  else put(file, opt + 28, 0x400000, 4);
  put(file, opt + 32, 0x1000, 4);
  put(file, opt + 36, 0x10, 4);
  put(file, opt + 56, 0x3000, 4);
  put(file, opt + 60, 0x200, 4);
  const size_t dir = opt + (wide ? 112 : 96);
  put(file, dir - 4, 6, 4);
  put(file, dir, 0x2000, 4); // Export directory
  put(file, dir + 4, 0x60, 4);
  put(file, dir + 40, 0x2060, 4); // Base relocation directory
  put(file, dir + 44, 12, 4);
  auto section = [&](size_t at, std::string_view name, unsigned size,
                     unsigned va, unsigned rawSize, unsigned ptr, unsigned flags) {
    text(file, at, name);
    put(file, at + 8, size, 4);
    put(file, at + 12, va, 4);
    put(file, at + 16, rawSize, 4);
    put(file, at + 20, ptr, 4);
    put(file, at + 36, flags, 4);
  };
  section(0xf8, ".text", 0x20, 0x1000, 0x10, 0x200, 0x60000020);
  section(0x120, ".edata", 0x80, 0x2000, 0x80, 0x240, 0x40000040);
  text(file, 0x200, "AUDIT123456789ab");
  put(file, 0x24c, 0x203a, 4); // Module name RVA
  put(file, 0x250, 1, 4); // Ordinal base
  put(file, 0x254, 1, 4); // Function count
  put(file, 0x258, 1, 4); // Name count
  put(file, 0x25c, 0x2030, 4);
  put(file, 0x260, 0x2034, 4);
  put(file, 0x264, 0x2038, 4);
  put(file, 0x270, 0x1000, 4); // Direct export RVA
  put(file, 0x274, 0x2044, 4); // Export name RVA
  text(file, 0x27a, "test.dll\0"sv);
  text(file, 0x284, "exported\0"sv);
  put(file, 0x2a0, 0x1000, 4);
  put(file, 0x2a4, 12, 4);
  put(file, 0x2a8, wide ? 0xa008 : 0x3008, 2); // DIR64 or HIGHLOW at offset 8
  return file;
}

inline Bytes machO(bool relocatable = false) {
  Bytes file(0x190);
  constexpr uint64_t base = 0x100000000;
  const uint64_t textAddress = relocatable ? 0x20 : base + 0x140;
  put(file, 0, 0xfeedfacf, 4); // Mach-O 64 little endian
  put(file, 4, 0x1000007, 4); // CPU_TYPE_X86_64
  put(file, 8, 3, 4);
  put(file, 12, relocatable ? 1 : 2, 4); // MH_OBJECT or MH_EXECUTE
  put(file, 16, 2, 4);
  put(file, 20, 256, 4);
  put(file, 32, 0x19, 4); // LC_SEGMENT_64
  put(file, 36, 232, 4);
  text(file, 40, "__TEXT"sv);
  put(file, 56, relocatable ? 0x20 : base, 8);
  put(file, 64, 0x300, 8);
  put(file, 72, 0x140, 8);
  put(file, 80, 8, 8);
  put(file, 92, 5, 4);
  put(file, 96, 2, 4);
  auto section = [&](size_t at, std::string_view name, uint64_t va,
                     unsigned raw, unsigned flags) {
    text(file, at, name);
    text(file, at + 16, "__TEXT"sv);
    put(file, at + 32, va, 8);
    put(file, at + 40, 8, 8);
    put(file, at + 48, raw, 4);
    put(file, at + 64, flags, 4);
  };
  section(104, "__text", textAddress, 0x140, 0x80000400);
  section(184, "__bss", textAddress + 0x20, 0, 1);
  put(file, 160, 0x148, 4);
  put(file, 164, 2, 4);
  put(file, 264, 2, 4); // LC_SYMTAB
  put(file, 268, 24, 4);
  put(file, 272, 0x158, 4);
  put(file, 276, 2, 4);
  put(file, 280, 0x178, 4);
  put(file, 284, 24, 4);
  text(file, 0x140, "AUDIT123"sv);
  put(file, 0x148, 0, 4);
  put(file, 0x14c, 0x0c000001, 4); // External 32-bit relocation to symbol 1
  put(file, 0x150, 4, 4);
  put(file, 0x154, 0x04000002, 4); // Local relocation refers to section ordinal 2
  put(file, 0x158, 1, 4);
  put(file, 0x15c, 0xf, 1); // N_SECT | N_EXT
  put(file, 0x15d, 1, 1);
  put(file, 0x160, textAddress, 8);
  put(file, 0x168, 9, 4);
  put(file, 0x16c, 1, 1); // N_UNDF | N_EXT
  text(file, 0x178, "\0_marker\0_missing\0"sv);
  return file;
}

inline Bytes fatMachO() {
  auto thin = machO();
  // FAT_CIGAM (little-endian table), one x86-64 slice at offset 0x20.
  Bytes file(0x20);
  put(file, 0, 0xcafebabe, 4);
  put(file, 4, 1, 4);
  put(file, 8, 0x1000007, 4);
  put(file, 12, 3, 4);
  put(file, 16, 0x20, 4);
  put(file, 20, thin.size(), 4);
  put(file, 24, 2, 4);
  file.insert(file.end(), thin.begin(), thin.end());
  return file;
}

}  // namespace fixture
