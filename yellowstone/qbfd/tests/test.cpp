#include <cstdlib>
#include <iostream>
#include <random>

#include <qBFD.hpp>
#include <qStrip.hpp>

using namespace qbfd;
using Bytes = std::vector<uint8_t>;
size_t otherTranslationUnit();
#define CHECK(...)                                                        \
  do {                                                                    \
    if (!(__VA_ARGS__)) {                                                 \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #__VA_ARGS__ "\n"; \
      std::abort();                                                       \
    }                                                                     \
  } while (false)
void put(Bytes& b, size_t off, uint64_t value, size_t width, bool big = false) {
  CHECK(off + width <= b.size());
  for (size_t i = 0; i < width; ++i)
    b[off + i] = uint8_t(value >> (8 * (big ? width - i - 1 : i)));
}
void text(Bytes& b, size_t off, std::string_view value) {
  CHECK(off + value.size() <= b.size());
  std::copy(value.begin(), value.end(), b.begin() + off);
}
Bytes elfFile(bool wide, bool big) {
  Bytes b(wide ? 64 : 52);
  b[0] = 0x7f;
  text(b, 1, "ELF");
  b[4] = wide ? 2 : 1;
  b[5] = big ? 2 : 1;
  b[6] = 1;
  put(b, 16, 1, 2, big);
  put(b, 18, wide ? 62 : 3, 2, big);
  put(b, 20, 1, 4, big);
  put(b, wide ? 52 : 40, b.size(), 2, big);
  return b;
}
Bytes elfTables(bool wide, bool big, bool extended = false) {
  auto b = elfFile(wide, big);
  b.resize(768);
  auto p = [&](size_t o, uint64_t v, size_t n) { put(b, o, v, n, big); };
  size_t stride = wide ? 64 : 40, sh = 256, word = wide ? 8 : 4;
  p(wide ? 40 : 32, sh, word);
  p(wide ? 58 : 46, stride, 2);
  p(wide ? 60 : 48, extended ? 0 : 5, 2);
  p(wide ? 62 : 50, extended ? 0xffff : 2, 2);
  if (extended) {
    p(sh + (wide ? 32 : 20), 5, word);
    p(sh + (wide ? 40 : 24), 2, 4);
  }
  auto section = [&](size_t i, size_t name, size_t type, size_t off,
                     size_t size, size_t link, size_t info, size_t entry) {
    size_t o = sh + i * stride;
    p(o, name, 4);
    p(o + 4, type, 4);
    p(o + 8, i == 1 ? 6 : 0, word);
    p(o + (wide ? 24 : 16), off, word);
    p(o + (wide ? 32 : 20), size, word);
    p(o + (wide ? 40 : 24), link, 4);
    p(o + (wide ? 44 : 28), info, 4);
    p(o + (wide ? 48 : 32), 1, word);
    p(o + (wide ? 56 : 36), entry, word);
  };
  std::string strings(
      "\0.text\0.strtab\0.symtab\0.rela.text\0external\0entry\0", 49);
  text(b, 600, strings);
  section(1, 1, 1, 64, 4, 0, 0, 0);
  section(2, 7, 3, 600, strings.size(), 0, 0, 0);
  size_t syment = wide ? 24 : 16;
  section(3, 15, 2, 128, syment * 3, 2, 1, syment);
  auto symbol = [&](size_t i, size_t name, size_t sec) {
    size_t o = 128 + i * syment;
    p(o, name, 4);
    b[o + (wide ? 4 : 12)] = 0x12;
    p(o + (wide ? 6 : 14), sec, 2);
  };
  symbol(1, 34, 0);
  symbol(2, 43, 1);
  size_t relent = wide ? 24 : 12;
  section(4, 23, 4, 208, relent, 3, 1, relent);
  p(208 + word, wide ? (uint64_t(1) << 32) | 2 : 0x102, word);
  p(208 + word * 2, UINT64_MAX - 3, word);
  return b;
}
Bytes coffFile(bool bigobj = false) {
  Bytes b(256);
  size_t sh = bigobj ? 56 : 20, symbol = 128, stride = bigobj ? 20 : 18;
  if (bigobj) {
    put(b, 2, 0xffff, 2);
    put(b, 4, 2, 2);
    put(b, 6, 0x8664, 2);
    std::copy(std::begin(coff::k::BigObjClassId),
              std::end(coff::k::BigObjClassId), b.begin() + 12);
    put(b, 44, 1, 4);
    put(b, 48, symbol, 4);
    put(b, 52, 1, 4);
  } else {
    put(b, 0, 0x8664, 2);
    put(b, 2, 1, 2);
    put(b, 8, symbol, 4);
    put(b, 12, 1, 4);
  }
  text(b, sh, ".text");
  put(b, sh + 16, 4, 4);
  put(b, sh + 20, 100, 4);
  put(b, sh + 24, 110, 4);
  put(b, sh + 32, 1, 2);
  put(b, sh + 36, 0x60000020, 4);
  put(b, 100, 0xfffffffc, 4);
  put(b, 118, 4, 2);
  text(b, symbol, "entry");
  put(b, symbol + 12, 1, bigobj ? 4 : 2);
  put(b, symbol + (bigobj ? 16 : 14), 0x20, 2);
  b[symbol + stride - 2] = 2;
  put(b, symbol + stride, 4, 4);
  return b;
}
Bytes peFile(bool wide) {
  Bytes b(1536);
  text(b, 0, "MZ");
  put(b, 0x3c, 64, 4);
  text(b, 64, "PE");
  size_t fh = 68, oh = 88, opt = wide ? 240 : 224, sh = oh + opt;
  put(b, fh, wide ? 0x8664 : 0x14c, 2);
  put(b, fh + 2, 1, 2);
  put(b, fh + 16, opt, 2);
  put(b, fh + 18, 0x2002, 2);
  put(b, oh, wide ? 0x20b : 0x10b, 2);
  put(b, oh + 16, 0x1000, 4);
  put(b, oh + (wide ? 24 : 28), wide ? 0x140000000ull : 0x400000, wide ? 8 : 4);
  put(b, oh + 32, 0x1000, 4);
  put(b, oh + 36, 0x200, 4);
  put(b, oh + 56, 0x2000, 4);
  put(b, oh + 60, 0x200, 4);
  put(b, oh + (wide ? 108 : 92), 16, 4);
  size_t dirs = oh + (wide ? 112 : 96);
  put(b, dirs, 0x1000, 4);
  put(b, dirs + 4, 0x100, 4);
  put(b, dirs + 8, 0x1100, 4);
  put(b, dirs + 12, 40, 4);
  put(b, dirs + 40, 0x1300, 4);
  put(b, dirs + 44, 12, 4);
  text(b, sh, ".text");
  put(b, sh + 8, 0x500, 4);
  put(b, sh + 12, 0x1000, 4);
  put(b, sh + 16, 0x400, 4);
  put(b, sh + 20, 0x200, 4);
  put(b, sh + 36, 0x60000020, 4);
  // Export directory: one named export, one forwarder, one hole.
  put(b, 0x20c, 0x1080, 4);
  put(b, 0x210, 1, 4);
  put(b, 0x214, 3, 4);
  put(b, 0x218, 1, 4);
  put(b, 0x21c, 0x1040, 4);
  put(b, 0x220, 0x1050, 4);
  put(b, 0x224, 0x1060, 4);
  put(b, 0x240, 0x1200, 4);
  put(b, 0x244, 0x1090, 4);
  put(b, 0x250, 0x1070, 4);
  text(b, 0x270, "entry");
  text(b, 0x280, "test.dll");
  text(b, 0x290, "other.func");
  // Import descriptor, lookup table, DLL and hint/name, and IAT.
  put(b, 0x300, 0x1140, 4);
  put(b, 0x30c, 0x1180, 4);
  put(b, 0x310, 0x1160, 4);
  size_t width = wide ? 8 : 4;
  put(b, 0x340, 0x1190, width);
  put(b, 0x340 + width, (uint64_t(1) << (width * 8 - 1)) | 7, width);
  text(b, 0x380, "dep.dll");
  put(b, 0x390, 3, 2);
  text(b, 0x392, "imported");
  put(b, 0x500, 0x1000, 4);
  put(b, 0x504, 12, 4);
  put(b, 0x508, wide ? 0xa200 : 0x3200, 2);
  return b;
}
Bytes machFile(bool wide, bool big) {
  size_t header = wide ? 32 : 28, segment = wide ? 72 : 56,
         section = wide ? 80 : 68;
  Bytes b(header + segment + section + 24 + 32);
  auto p = [&](size_t o, uint64_t v, size_t n) { put(b, o, v, n, big); };
  p(0, wide ? 0xfeedfacf : 0xfeedface, 4);
  p(4, wide ? 0x1000007 : 7, 4);
  p(12, 1, 4);
  p(16, 2, 4);
  p(20, segment + section + 24, 4);
  size_t o = header, sec = o + segment, data = header + segment + section + 24;
  p(o, wide ? 0x19 : 1, 4);
  p(o + 4, segment + section, 4);
  text(b, o + 8, "__TEXT");
  p(o + (wide ? 32 : 28), 4, wide ? 8 : 4);
  p(o + (wide ? 40 : 32), data, wide ? 8 : 4);
  p(o + (wide ? 48 : 36), 4, wide ? 8 : 4);
  p(o + (wide ? 60 : 44), 5, 4);
  p(o + (wide ? 64 : 48), 1, 4);
  text(b, sec, "__text");
  text(b, sec + 16, "__TEXT");
  p(sec + (wide ? 40 : 36), 4, wide ? 8 : 4);
  p(sec + (wide ? 48 : 40), data, 4);
  p(sec + (wide ? 64 : 56), 0x80000400, 4);
  size_t symcmd = header + segment + section;
  p(symcmd, 2, 4);
  p(symcmd + 4, 24, 4);
  p(symcmd + 8, data + 4, 4);
  p(symcmd + 12, 1, 4);
  size_t strings = data + 4 + (wide ? 16 : 12);
  p(symcmd + 16, strings, 4);
  p(symcmd + 20, 8, 4);
  p(data + 4, 1, 4);
  b[data + 8] = 0xf;
  b[data + 9] = 1;
  text(b, strings + 1, "_entry");
  return b;
}
Bytes sectionlessElf(bool gnuHash) {
  auto b = elfFile(true, false);
  b.resize(784);
  constexpr uint64_t base = 0x400000;
  auto p = [&](size_t o, uint64_t v, size_t n) { put(b, o, v, n); };
  p(16, 3, 2);
  p(24, base + 720, 8);
  p(32, 64, 8);
  p(54, 56, 2);
  p(56, 2, 2);
  auto program = [&](uint64_t off, uint32_t type, uint64_t data, uint64_t size,
                     uint64_t mem) {
    p(off, type, 4); p(off + 4, 5, 4);
    p(off + 8, data, 8); p(off + 16, base + data, 8);
    p(off + 32, size, 8); p(off + 40, mem, 8); p(off + 48, 1, 8);
  };
  program(64, 1, 0, b.size(), 1024);
  program(120, 2, 176, 16 * 12, 16 * 12);
  constexpr uint64_t strings = 384, syms = 480, hash = 584, versym = 624,
                     verdef = 640, verneed = 672;
  size_t next = 176;
  auto tag = [&](uint64_t id, uint64_t value) {
    p(next, id, 8); p(next + 8, value, 8); next += 16;
  };
  tag(5, base + strings); tag(10, 96);
  tag(1, 1); tag(14, 9);
  tag(6, base + syms); tag(11, 24);
  tag(gnuHash ? 0x6ffffef5 : 4, base + hash);
  tag(0x6ffffff0, base + versym);
  tag(0x6ffffffc, base + verdef); tag(0x6ffffffd, 1);
  tag(0x6ffffffe, base + verneed); tag(0x6fffffff, 1);
  // The DT_NULL entry follows the 12 real tags.
  p(120 + 32, 16 * 13, 8); p(120 + 40, 16 * 13, 8);
  constexpr char names[] = "\0dep.so\0\0sample.so\0\0provided\0external\0VER_1\0VER_2\0";
  text(b, strings, std::string_view(names, sizeof(names) - 1));
  auto symbol = [&](size_t i, uint32_t name, uint8_t binding, uint16_t section,
                    uint64_t value) {
    size_t o = syms + i * 24;
    p(o, name, 4); p(o + 4, binding, 1); p(o + 6, section, 2);
    p(o + 8, value, 8);
  };
  symbol(1, 29, 0x22, 0, 0);
  symbol(2, 20, 0x12, 1, base + 720);
  symbol(3, 20, 0x12, 1, base + 724);
  if (gnuHash) {
    p(hash, 1, 4); p(hash + 4, 2, 4); p(hash + 8, 1, 4);
    p(hash + 24, 2, 4); p(hash + 32, 1, 4);
  } else {
    p(hash, 1, 4); p(hash + 4, 4, 4);
  }
  p(versym + 2, 3, 2); p(versym + 4, 2, 2); p(versym + 6, 0x8002, 2);
  p(verdef, 1, 2); p(verdef + 4, 2, 2); p(verdef + 6, 1, 2);
  p(verdef + 12, 20, 4); p(verdef + 20, 38, 4);
  p(verneed, 1, 2); p(verneed + 2, 1, 2); p(verneed + 4, 1, 4);
  p(verneed + 8, 16, 4); p(verneed + 22, 3, 2); p(verneed + 24, 44, 4);
  b[720] = 0xc3;
  return b;
}
Bytes machExports(bool dyldInfo) {
  Bytes b(260);
  auto p = [&](size_t o, uint64_t v, size_t n) { put(b, o, v, n); };
  p(0, 0xfeedfacf, 4); p(4, 0x100000c, 4);
  p(12, 6, 4); p(16, 3, 4);
  p(20, 72 + 40 + (dyldInfo ? 48 : 16), 4);
  p(32, 0x19, 4); p(36, 72, 4);
  text(b, 40, "__TEXT"); p(56, 0x100000000, 8);
  p(64, 512, 8); p(80, b.size(), 8); p(92, 5, 4);
  p(104, 0x8000001f, 4); p(108, 40, 4); p(112, 24, 4);
  text(b, 128, "dep.dylib");
  p(144, dyldInfo ? 0x80000022 : 0x80000033, 4);
  p(148, dyldInfo ? 48 : 16, 4);
  size_t dir = dyldInfo ? 184 : 152;
  p(dir, 200, 4);
  const Bytes trie = {0, 2, '_', 'p', 0, 10, '_', 'f', 0, 15,
                      3, 4, 0xe0, 1, 0, 5, 8, 1, '_', 't', 0, 0};
  p(dir + 4, trie.size(), 4);
  std::copy(trie.begin(), trie.end(), b.begin() + 200);
  return b;
}
void normalizedTests() {
  for (bool gnu : {false, true}) {
    auto b = sectionlessElf(gnu);
    auto obj = qbfd::open(b);
    if (!obj) std::cerr << obj.error().message << '\n';
    CHECK(obj);
    const auto& f = *obj->object;
    auto info = imageInfo(f);
    CHECK(info.format == Format::ELF && info.arch == Arch::X86_64 &&
          info.addressBits == 64 && info.abi == abi::CallConv::SysV_AMD64 &&
          info.kind == ImageKind::SharedLibrary);
    CHECK(f.sections().empty() && f.symbols().empty());
    CHECK(f.dynamicSymbols().size() == 4 && f.soname() == "sample.so");
    CHECK(f.neededLibraries() == std::vector<std::string>{"dep.so"});
    auto ex = exports(f), im = imports(f);
    CHECK(ex.size() == 2 && ex[0].name == "provided" && ex[0].version == "VER_1" &&
          ex[0].defaultVersion && !ex[1].defaultVersion && ex[0].address == info.entryPoint);
    CHECK(im.size() == 1 && im[0].name == "external" && im[0].weak &&
          !im[0].address && im[0].version == "VER_2" && im[0].library == "dep.so");
    CHECK(f.fileOffsetForRva(720) == 720 &&
          f.addressForFileOffset(720) == info.entryPoint);
    CHECK(f.bytesAtRva(720, 1)->front() == 0xc3);
    CHECK(!f.bytesAtRva(720, UINT64_MAX) && !f.fileOffsetForRva(UINT64_MAX));
    CHECK(!f.fileOffsetForRva(784) && !f.bytesAtRva(783, 2));
    // Truncated/unmapped symbol table and unrecognized symbol version ID.
    put(b, 176 + 4 * 16 + 8, info.imageBase + 780, 8);
    CHECK(!qbfd::open(b));
    b = sectionlessElf(gnu);
    put(b, 626, 22, 2);
    CHECK(!qbfd::open(b));
    // Change only the ELF machine/flags to exercise conservative ABI selection.
    b = sectionlessElf(gnu);
    put(b, 18, 243, 2);
    auto soft = qbfd::open(b);
    CHECK(soft && imageInfo(*soft->object).abi == abi::CallConv::Unknown);
    put(b, 48, 5, 4);  // EF_RISCV_RVC | EF_RISCV_FLOAT_ABI_DOUBLE.
    auto hard = qbfd::open(b);
    CHECK(hard && imageInfo(*hard->object).abi == abi::CallConv::Riscv64);
  }
  for (bool legacy : {false, true}) {
    auto b = machExports(legacy);
    auto obj = qbfd::openMachOSlice(b, Arch::AArch64);
    CHECK(obj);
    const auto& f = *obj->object;
    CHECK(imageInfo(f).abi == abi::CallConv::DarwinArm64);
    CHECK(f.symbols().empty() && f.dynamicSymbols().size() == 2);
    auto ex = exports(f);
    CHECK(ex[0].name == "_p" && ex[0].weak && ex[0].address == 0x1000000e0);
    CHECK(ex[1].name == "_f" && ex[1].forwarded && !ex[1].address &&
          ex[1].library == "dep.dylib" && ex[1].target == "_t");
    CHECK(f.fileOffsetForRva(224) == 224);
    b[205] = 0;  // root as child: cycle.
    CHECK(!qbfd::open(b));
    b = machExports(legacy);
    b[217] = 2;  // dylib ordinal beyond load commands.
    CHECK(!qbfd::open(b));
  }
  CHECK(abi::parseCallConv("darwin_arm64") == abi::CallConv::DarwinArm64);
  CHECK(!abi::parseCallConv("not_an_abi"));
}
void exercise(const Bytes& bytes) {
  auto result = qbfd::open(bytes);
  if (!result) std::cerr << result.error().message << "\n";
  CHECK(result);
  auto& obj = *result->object;
  for (const auto& section : obj.sections()) {
    CHECK(obj.sectionContents(section));
    CHECK(obj.relocations(section));
  }
  auto names = obj | query::sections() |
               query::where(query::flags(sec::Code) & !query::flags(sec::Bss)) |
               query::transform([](const Section& s) { return s.name; });
  (void)names;
  auto fromPipeline = bytes | query::read() |
                      query::requireFormat(obj.format()) |
                      query::inspect(query::sections());
  CHECK(fromPipeline && fromPipeline->size() == obj.sections().size());
  // Every prefix must be safe, including ones which remain valid with omitted
  // trailing padding.
  for (size_t n = 0; n < bytes.size(); ++n) {
    Bytes truncated(bytes.begin(), bytes.begin() + n);
    (void)qbfd::open(std::move(truncated));
  }
  // Mutate structure fields to exercise overflow and index handling under
  // sanitizers.
  std::mt19937 rng(42);
  for (int i = 0; i < 500; ++i) {
    auto mutated = bytes;
    for (int j = 0; j < 4; ++j)
      mutated[rng() % mutated.size()] = uint8_t(rng());
    auto parsed = qbfd::open(std::move(mutated));
    if (parsed)
      for (const auto& s : parsed->object->sections())
        CHECK(parsed->object->relocations(s));
  }
}
// Builds an `ar` container by hand, so the reader is tested against bytes it
// did not write itself.
Bytes arHeader(std::string_view name, uint64_t size) {
  std::string h(60, ' ');
  for (size_t i = 0; i < name.size() && i < 16; ++i) h[i] = name[i];
  auto field = [&](size_t at, size_t width, const std::string& value) {
    for (size_t i = 0; i < value.size() && i < width; ++i) h[at + i] = value[i];
  };
  field(16, 12, "0");
  field(28, 6, "0");
  field(34, 6, "0");
  field(40, 8, "644");
  field(48, 10, std::to_string(size));
  h[58] = '`';
  h[59] = '\n';
  return Bytes(h.begin(), h.end());
}

void archiveTests(const Bytes& object) {
  // A two-member archive with a long-name table, a symbol index and an odd
  // first member so the pad byte is exercised.
  Bytes ar = Bytes{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
  auto append = [&](std::string_view name, std::string_view payload) {
    auto header = arHeader(name, payload.size());
    ar.insert(ar.end(), header.begin(), header.end());
    ar.insert(ar.end(), payload.begin(), payload.end());
    if (payload.size() & 1) ar.push_back('\n');
  };
  const std::string longName = "a_long_member_name.o";
  append("//", longName + "/\n");
  append("/0", std::string_view(reinterpret_cast<const char*>(object.data()),
                                object.size()));
  auto opened = openArchive(ar, "test.ar");
  CHECK(opened);
  const auto& members = opened->archive->members();
  CHECK(members.size() == 2);
  // The long-name table is metadata, and keeps its own conventional name.
  CHECK(members[0].name == "//");
  CHECK(members[0].special);
  CHECK(members[1].name == longName);
  CHECK(!members[1].special);
  CHECK(members[1].size == object.size());
  CHECK(opened->archive->memberData(members[1]).size() == object.size());
  auto member = opened->archive->openMember(members[1]);
  CHECK(member);
  // A metadata member is not an object.
  CHECK(!opened->archive->openMember(members[0]));
  CHECK(opened->archive->findMember(longName) == &members[1]);
  CHECK(opened->archive->findMember("//") == &members[0]);
  // Nothing here carries an index.
  CHECK(opened->archive->symbolIndex().empty());

  // Truncation at every point must be reported, never read out of bounds.
  for (size_t n = 0; n < ar.size(); ++n) {
    Bytes cut(ar.begin(), ar.begin() + n);
    (void)openArchive(std::move(cut));
  }
  // So must a header with a mangled terminator.
  Bytes bad = ar;
  bad[8 + 58] = 'X';
  CHECK(!openArchive(std::move(bad)));
  // And a member whose declared size runs past the end of the container.
  Bytes over = ar;
  over[8 + 48] = '9';
  over[8 + 49] = '9';
  over[8 + 50] = '9';
  CHECK(!openArchive(std::move(over)));

  // Round trip: build an archive, read it back, and check the member set and
  // the index survive.
  ArchiveOptions options;
  options.index = true;
  std::vector<ArchiveEntry> entries = {
      {"short.o", Bytes{1, 2, 3}},
      {longName, Bytes{4, 5, 6, 7}},
  };
  auto built = buildArchive(entries, options);
  CHECK(built);
  auto reread = openArchive(*built, "built.ar");
  CHECK(reread);
  // Three entries: the long-name table plus the two members. Metadata is
  // reported by members() and flagged, not hidden.
  CHECK(reread->archive->members().size() == 3);
  CHECK(reread->archive->members()[0].special);
  CHECK(reread->archive->findMember("short.o"));
  CHECK(reread->archive->findMember(longName));
  CHECK(reread->archive->readMember(*reread->archive->findMember(longName)));
  // Deterministic output must be byte-identical across runs.
  auto again = buildArchive(entries, options);
  CHECK(again && *again == *built);
  // A payload that is not an object contributes no index entries but must not
  // break the build.
  auto symbols = archiveExportedSymbols(entries[0]);
  CHECK(symbols && symbols->empty());
  // The Windows flavour is readable but not writable.
  ArchiveOptions windows;
  windows.flavor = ArchiveFlavor::Windows;
  CHECK(!buildArchive(entries, windows));
}

// Stripping must leave a file that still parses, keeps the symbols a linker
// needs, and does not grow without need.
void stripTests() {
  auto source = elfTables(true, false);
  for (auto mode : {strip::Mode::All, strip::Mode::Debug, strip::Mode::Unneeded}) {
    strip::Options options;
    options.mode = mode;
    auto stripped = strip::strip(source, options);
    CHECK(stripped);
    auto back = open(stripped->bytes);
    CHECK(back);
    // Every section and every relocation must still be readable.
    // A relocation may only name a symbol that still exists, and every section
    // and table must still be readable.
    for (const auto& s : back->object->sections()) {
      CHECK(back->object->sectionContents(s));
      auto relocs = back->object->relocations(s);
      CHECK(relocs);
      for (const auto& r : *relocs)
        if (r.symbol) CHECK(*r.symbol < back->object->symbols().size());
    }
  }
  // Mode::All removes the symbol table outright; the file must still be a
  // well-formed object with no dangling references.
  strip::Options all;
  all.mode = strip::Mode::All;
  auto stripped = strip::strip(source, all);
  CHECK(stripped);
  CHECK(stripped->symbolsRemoved > 0);
  auto back = open(stripped->bytes);
  CHECK(back);
  CHECK(back->object->symbols().empty());
  for (const auto& s : back->object->sections())
    CHECK(back->object->relocations(s));
  // -R on a section that exists removes it, along with the relocations that
  // targeted it.
  strip::Options remove;
  remove.mode = strip::Mode::Debug;
  remove.removeSections = {".text"};
  auto withoutText = strip::strip(source, remove);
  CHECK(withoutText);
  CHECK(withoutText->sectionsRemoved > 0);
  auto reopened = open(withoutText->bytes);
  CHECK(reopened);
  CHECK(!reopened->object->findSection(".text"));
  CHECK(reopened->object->findSection(".rela.text") == nullptr);
  for (const auto& s : reopened->object->sections()) {
    // Bind the result before iterating: reading through a temporary makes GCC
    // emit a std::variant false positive on Expected.
    auto relocs = reopened->object->relocations(s);
    CHECK(relocs);
    for (const auto& r : *relocs)
      if (r.symbol) CHECK(*r.symbol < reopened->object->symbols().size());
  }
  // -R naming a section that is not there leaves the file's sections alone.
  // sectionsRemoved is a count of original sections that no longer exist, which
  // includes the two name tables this pass rebuilds, so the behaviour to check
  // is which sections survive rather than the exact total.
  strip::Options absent;
  absent.mode = strip::Mode::Debug;
  absent.removeSections = {".no_such_section"};
  auto unchanged = strip::strip(source, absent);
  CHECK(unchanged);
  auto same = open(unchanged->bytes);
  CHECK(same);
  auto original = open(source);
  CHECK(original);
  for (const auto& s : original->object->sections()) {
    // The two name tables are rebuilt as fresh sections, so the old slots are
    // not expected to survive under their original names.
    if (s.nativeType == 3 /* SHT_STRTAB */) continue;
    CHECK(same->object->findSection(s.name));
  }
  // -K overrides -R for the same name.
  strip::Options kept;
  kept.mode = strip::Mode::All;
  kept.removeSections = {".text"};
  kept.keepSections = {".text"};
  auto withText = strip::strip(source, kept);
  CHECK(withText);
  auto withTextFile = open(withText->bytes);
  CHECK(withTextFile);
  CHECK(withTextFile->object->findSection(".text"));
  // The glob matcher is used by -R/-K/-N, so it is worth pinning down.
  CHECK(strip::globMatch(".d*", ".debug_info"));
  CHECK(strip::globMatch("a?c", "abc"));
  CHECK(!strip::globMatch("a?c", "abbc"));
  CHECK(strip::globMatch("*", ""));
}

int main(int argc, char** argv) {
  normalizedTests();
  CHECK(otherTranslationUnit() > 0);
  auto count = Registry::instance().targets().size();
  registerBuiltinTargets();
  CHECK(Registry::instance().targets().size() == count);
  CHECK(!qbfd::open(Bytes{}));
  auto failure = Bytes{} | query::read() | query::inspect(query::sections());
  CHECK(!failure);
  auto pred = query::named("a") | query::named("b");
  Section test;
  test.name = "b";
  CHECK(pred(test));
  for (bool wide : {false, true})
    for (bool big : {false, true}) {
      auto e = elfFile(wide, big);
      exercise(e);
      for (bool extended : {false, true}) {
        auto tables = elfTables(wide, big, extended);
        exercise(tables);
        auto f = qbfd::open(tables);
        CHECK(f);
        CHECK(f->object->sections().size() == 5);
        CHECK(f->object->findSection(".text"));
        CHECK(f->object->findSymbol("entry") &&
              f->object->findSymbol("external"));
        auto rel = f->object->relocations(f->object->sections()[1]);
        CHECK(rel && rel->size() == 1 && (*rel)[0].symbol == 1 &&
              (*rel)[0].addend == -4);
      }
      auto m = machFile(wide, big);
      exercise(m);
      auto obj = qbfd::open(m);
      CHECK(obj->object->symbols().front().name == "_entry");
      CHECK(obj->object->endian() == (big ? Endian::Big : Endian::Little));
    }
  for (bool big : {false, true}) {
    auto bytes = coffFile(big);
    exercise(bytes);
    auto obj = qbfd::open(bytes);
    auto& file = *obj->object;
    CHECK(file.symbols().front().name == "entry");
    auto rel = file.relocations(file.sections().front());
    CHECK(rel && rel->size() == 1);
    CHECK(rel->front().addend == -4 && rel->front().hasAddend);
    bytes[128 + (big ? 19 : 17)] = 2;
    CHECK(!qbfd::open(bytes));
  }
  for (bool wide : {false, true}) {
    auto bytes = peFile(wide);
    exercise(bytes);
    auto obj = qbfd::open(bytes);
    auto& file = dynamic_cast<const pe::PEFile&>(*obj->object);
    CHECK(file.imports().size() == 2 && file.exports().size() == 2);
    CHECK(file.imports()[0].name == "imported" &&
          file.imports()[1].ordinal == 7);
    CHECK(file.exports()[1].forwarder == "other.func");
    CHECK(file.neededLibraries() == std::vector<std::string>{"dep.dll"});
    auto ex = exports(file), im = imports(file);
    CHECK(ex[1].name.empty() && ex[1].ordinal == 2 && ex[1].forwarded &&
          !ex[1].address && ex[1].library == "other" && ex[1].target == "func");
    CHECK(im[1].name.empty() && im[1].ordinal == 7 && im[1].address &&
          im[1].library == "dep.dll");
    CHECK(file.fileOffsetForRva(0) == 0 && file.bytesAtRva(0, 2)->front() == 'M');
    CHECK(file.addressForFileOffset(0x400) == file.imageBase() + 0x1200);
    CHECK(!file.vmaToFileOffset(file.imageBase() + 0x1401));
    auto rel = file.relocations(file.sections()[0]);
    CHECK(rel && rel->size() == 1);
    put(bytes, 68 + 16, 2, 2);
    CHECK(!qbfd::open(bytes));
  }
  {
    auto bytes = elfTables(true, false);
    // Symbol-table link, relocation symbol index, and raw section extent.
    put(bytes, 256 + 3 * 64 + 40, 99, 4);
    CHECK(!qbfd::open(bytes));
    bytes = elfTables(true, false);
    put(bytes, 216, uint64_t(99) << 32, 8);
    CHECK(!qbfd::open(bytes));
    bytes = elfTables(true, false);
    put(bytes, 256 + 64 + 24, UINT64_MAX, 8);
    CHECK(!qbfd::open(bytes));
    auto result = qbfd::open(elfTables(true, false));
    auto owned = std::move(result->object);
    result->buffer.reset();
    CHECK(owned->sectionContents(owned->sections()[1])->size() == 4);
    Section invalid;
    invalid.index = UINT32_MAX;
    CHECK(!owned->sectionContents(invalid));
    CHECK(!owned->relocations(invalid));
  }
  {
    auto b = peFile(true);
    put(b, 0x360, 0, 2);
    put(b, 0x260, 99, 2);
    CHECK(!qbfd::open(b));  // export ordinal exceeds EAT
    b = peFile(true);
    put(b, 0x504, 0, 4);
    CHECK(!qbfd::open(b));
    b = coffFile();
    put(b, 110 + 4, 99, 4);
    CHECK(!qbfd::open(b));
    b = machFile(true, false);
    put(b, 36, 0, 4);
    CHECK(!qbfd::open(b));
  }
  {
    auto bytes = elfTables(true, false);
    put(bytes, 18, 8, 2);
    put(bytes, 216, 1, 4);
    put(bytes, 220, 2, 4, true);
    auto result = qbfd::open(bytes);
    CHECK(result);
    auto relocations =
        result->object->relocations(result->object->sections()[1]);
    CHECK(relocations && relocations->at(0).symbol == 1 &&
          relocations->at(0).nativeType == 2);
  }
  archiveTests(elfTables(true, false));
  stripTests();
  auto thin = machFile(true, false);
  Bytes fat(256 + thin.size());
  put(fat, 0, 0xcafebabe, 4, true);
  put(fat, 4, 1, 4, true);
  put(fat, 8, 0x1000007, 4, true);
  put(fat, 16, 256, 4, true);
  put(fat, 20, thin.size(), 4, true);
  put(fat, 24, 8, 4, true);
  std::copy(thin.begin(), thin.end(), fat.begin() + 256);
  CHECK(detectFormat(fat) == Format::MachO);
  CHECK(macho::slices(fat));
  CHECK(macho::openSlice(fat, 0));
  CHECK(!macho::openSlice(fat, 1));
  auto selected = qbfd::openMachOSlice(fat, Arch::X86_64);
  CHECK(selected && selected->object->data().size() == thin.size());
  CHECK(!qbfd::openMachOSlice(fat, Arch::AArch64));
  auto universal = qbfd::open(fat);
  CHECK(!universal && universal.error().code == Error::Code::Unsupported);
  for (int i = 1; i < argc; ++i) {
    auto buffer = Buffer::fromFile(argv[i]);
    CHECK(buffer);
    if (isArchive((*buffer)->span())) {
      // A container: every member must open, and the index must name members
      // that exist.
      auto archive = openArchive(argv[i]);
      CHECK(archive);
      size_t objects = 0;
      for (const auto& member : archive->archive->members()) {
        if (member.special) continue;
        ++objects;
        auto opened = archive->archive->openMember(member);
        CHECK(opened);
        for (const auto& s : (*opened)->sections()) {
          CHECK((*opened)->sectionContents(s));
          CHECK((*opened)->relocations(s));
        }
      }
      CHECK(objects > 0);
      for (const auto& e : archive->archive->symbolIndex())
        CHECK(archive->archive->memberAt(e.headerOffset));
      continue;
    }
    exercise(Bytes((*buffer)->span().begin(), (*buffer)->span().end()));
    auto opened = qbfd::open(std::filesystem::path(argv[i]));
    CHECK(opened);
    auto& file = *opened->object;
    for (const auto* name : {"external", "data", "zero", "exported"}) {
      const auto* symbol = file.findSymbol(name);
      if (!symbol) symbol = file.findSymbol(std::string("_") + name);
      CHECK(symbol);
      if (std::string_view(name) == "external")
        CHECK(symbol->flags & sym::Undefined);
      if (std::string_view(name) == "zero") {
        CHECK(symbol->section);
        CHECK(file.sections()[*symbol->section].flags & sec::Bss);
      }
      if (std::string_view(name) == "data") {
        CHECK(symbol->section);
        const auto& section = file.sections()[*symbol->section];
        auto contents = file.sectionContents(section);
        CHECK(contents);
        detail::Reader reader{*contents, file.endian()};
        CHECK(reader.u32(symbol->value - section.vma) == 9);
      }
    }
    size_t relocations = 0;
    for (const auto& section : file.sections())
      relocations += file.relocations(section)->size();
    CHECK(relocations > 0);
  }
  std::cout << "qBFD regression tests passed\n";
}
