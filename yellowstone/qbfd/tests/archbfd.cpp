#include "ArchBFDApp.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>

extern "C" int archbfd_test_c(const void*, size_t);
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " #c "\n"; std::abort(); } } while (0)
using Bytes = std::vector<uint8_t>;

static void put(Bytes& b, size_t at, uint64_t value, size_t width, bool big = false) {
  CHECK(at <= b.size() && width <= b.size() - at);
  for (size_t i = 0; i < width; ++i)
    b[at + i] = uint8_t(value >> (8 * (big ? width - i - 1 : i)));
}
static void text(Bytes& b, size_t at, std::string_view value) {
  CHECK(at <= b.size() && value.size() <= b.size() - at);
  std::copy(value.begin(), value.end(), b.begin() + at);
}
static Bytes coff() {
  Bytes b(256);
  put(b, 0, 0x8664, 2); put(b, 2, 2, 2);
  put(b, 8, 128, 4); put(b, 12, 1, 4);
  text(b, 20, ".text"); put(b, 32, 0x1000, 4);
  put(b, 36, 4, 4); put(b, 40, 100, 4); put(b, 44, 110, 4);
  put(b, 52, 1, 2); put(b, 56, 0x60000020, 4);
  text(b, 60, ".bss"); put(b, 72, 0x2000, 4);
  put(b, 76, 8, 4); put(b, 96, 0xc0000080, 4);
  put(b, 100, 0xfffffffc, 4); put(b, 110, 0x1000, 4); put(b, 118, 4, 2);
  text(b, 128, "entry"); put(b, 140, 1, 2); put(b, 142, 0x20, 2);
  b[144] = 2; put(b, 146, 4, 4);
  return b;
}
static Bytes archive(const Bytes& object) {
  Bytes b(68 + object.size(), ' ');
  text(b, 0, "!<arch>\n"); text(b, 8, "example.obj/");
  text(b, 24, "0"); text(b, 36, "0"); text(b, 42, "0");
  text(b, 48, "100644"); text(b, 56, std::to_string(object.size()));
  text(b, 66, "`\n");
  std::copy(object.begin(), object.end(), b.begin() + 68);
  if (b.size() & 1) b.push_back('\n');
  return b;
}
template <class F>
static void throws(archbfd_status status, F&& f) {
  try { f(); CHECK(false); }
  catch (const archbfd::Error& e) { CHECK(e.code() == status); }
}

int main() {
  static_assert(!std::is_copy_constructible_v<archbfd::Binary>);
  static_assert(std::is_nothrow_move_constructible_v<archbfd::Binary>);
  auto fixture = coff();
  int c = archbfd_test_c(fixture.data(), fixture.size());
  if (c) { std::cerr << "C ABI test failed at line " << c << '\n'; return 1; }
  auto object = archbfd::Binary::fromMemory(fixture);
  auto name = object.section(0).name;
  auto moved = std::move(object);
  CHECK(!object && moved);
  std::fill(fixture.begin(), fixture.end(), 0);
  CHECK(archbfd::text(name) == ".text" && moved.sectionBytes(0)[0] == 0xfc);
  CHECK(moved.sections().size() == 2 && moved.symbols().size() == 1);
  CHECK(moved.relocations(0)[0].addend == -4);
  CHECK(moved.segments().empty() && moved.dependencies().empty());
  throws(ARCHBFD_INVALID_ARGUMENT, [&] { object.info(); });
  throws(ARCHBFD_OUT_OF_RANGE, [&] { moved.section(SIZE_MAX); });
  throws(ARCHBFD_INVALID_ARGUMENT, [&] { moved.symbols(42); });
  throws(ARCHBFD_UNKNOWN_FORMAT, [&] { archbfd::Binary::fromMemory(fixture); });

  auto arBytes = archive(coff());
  archbfd::Binary member;
  {
    auto ar = archbfd::Archive::fromMemory(arBytes);
    CHECK(ar.size() == 1 && archbfd::text(ar.member(0).name) == "example.obj");
    member = ar.openMember(0);
    throws(ARCHBFD_OUT_OF_RANGE, [&] { ar.openMember(1); });
  }
  arBytes.clear(); arBytes.shrink_to_fit();
  CHECK(member.info().symbol_count == 1 && archbfd::text(member.symbol(0).name) == "entry");

  // ELF32, big-endian, sectionless header: verifies independent byte-order metadata.
  Bytes elf(52);
  text(elf, 0, std::string_view("\x7f" "ELF\x01\x02\x01", 7));
  put(elf, 16, 2, 2, true); put(elf, 18, 20, 2, true);
  put(elf, 20, 1, 4, true); put(elf, 24, 0x12345678, 4, true);
  put(elf, 40, 52, 2, true);
  auto e = archbfd::Binary::fromMemory(elf);
  CHECK(e.info().endian == ARCHBFD_BIG_ENDIAN && e.info().arch == ARCHBFD_ARCH_POWERPC);
  CHECK(e.info().entry_point == 0x12345678 && e.info().address_bits == 32);

  // A fat Mach-O table in big-endian wrapping one little-endian thin object.
  Bytes fat(64);
  put(fat, 0, 0xcafebabe, 4, true); put(fat, 4, 1, 4, true);
  put(fat, 8, 0x1000007, 4, true); put(fat, 16, 32, 4, true);
  put(fat, 20, 32, 4, true); put(fat, 24, 5, 4, true);
  put(fat, 32, 0xfeedfacf, 4); put(fat, 36, 0x1000007, 4); put(fat, 44, 1, 4);
  CHECK(archbfd::machoSlices(fat).size() == 1);
  auto slice = archbfd::Binary::machoSlice(fat, 0);
  CHECK(slice.info().format == ARCHBFD_FORMAT_MACHO && slice.data().size() == 32);
  throws(ARCHBFD_UNSUPPORTED, [&] { archbfd::Binary::fromMemory(fat); });
  throws(ARCHBFD_OUT_OF_RANGE, [&] { archbfd::Binary::machoSlice(fat, 1); });
  put(fat, 8, 12, 4, true);
  throws(ARCHBFD_MALFORMED, [&] { archbfd::Binary::machoSlice(fat, 0); });

  // Truncated inputs must never return a partially published handle.
  auto source = coff();
  for (size_t n = 0; n < source.size(); ++n) {
    archbfd_object* handle = nullptr;
    archbfd_error error{};
    auto status = archbfd_open_memory(source.data(), n, nullptr, &handle, &error);
    if (status == ARCHBFD_OK) {
      CHECK(handle); archbfd_close(handle);
    } else {
      CHECK(!handle && error.code == status && error.message[0]);
    }
  }

  // Exercise the filesystem entry points and the application traversal helper.
  auto filePath = std::filesystem::path("archbfd-test-input.a");
  {
    auto blob = archive(coff());
    std::ofstream out(filePath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
    CHECK(bool(out));
  }
  size_t visits = 0;
  archbfd::visitObjects(filePath, [&](std::string_view display, const archbfd::Binary& f) {
    CHECK(display.find("example.obj") != std::string_view::npos);
    CHECK(f.info().section_count == 2); ++visits;
  });
  CHECK(visits == 1);
  std::filesystem::remove(filePath);
  std::cout << "ArchBFD C/C++ ABI tests passed\n";
}
