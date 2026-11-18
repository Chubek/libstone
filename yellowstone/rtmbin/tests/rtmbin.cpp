#include <rtmbin/rtmbin.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <utility>

namespace {
using namespace rtmbin;
using S = AttributionStatus;
using Bytes = std::vector<std::uint8_t>;
using u64 = std::uint64_t;
constexpr auto max_u64 = std::numeric_limits<u64>::max();

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::cerr << __LINE__ << ": failed " << #expression << '\n'; \
    std::exit(1); \
  } \
} while (false)

void put(Bytes& bytes, std::size_t offset, u64 value, unsigned width = 4, bool be = false) {
  CHECK(offset + width <= bytes.size());
  for (unsigned i = 0; i < width; ++i)
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * (be ? width - 1 - i : i)));
}

void text(Bytes& bytes, std::size_t offset, std::string_view value) {
  CHECK(offset + value.size() <= bytes.size());
  std::copy(value.begin(), value.end(), bytes.begin() + offset);
}

// ELF64 with a readable segment, a separate executable load segment, and a
// non-load program header inside the text range.
Bytes elf64(u64 text_address = 0x402000, u64 text_mem = 8, u64 text_file = 4) {
  Bytes bytes(0x204);
  text(bytes, 0, "\x7f" "ELF");
  bytes[4] = 2; bytes[5] = 1; bytes[6] = 1;
  put(bytes, 16, 2, 2); // ET_EXEC
  put(bytes, 18, 62, 2);
  put(bytes, 20, 1);
  put(bytes, 32, 64, 8);
  put(bytes, 52, 64, 2);
  put(bytes, 54, 56, 2);
  put(bytes, 56, 3, 2);
  const auto segment = [&](std::size_t o, unsigned type, unsigned flags,
                           u64 address, u64 offset, u64 file, u64 mem) {
    put(bytes, o, type);
    put(bytes, o + 4, flags);
    put(bytes, o + 8, offset, 8);
    put(bytes, o + 16, address, 8);
    put(bytes, o + 32, file, 8);
    put(bytes, o + 40, mem, 8);
    put(bytes, o + 48, 1, 8);
  };
  segment(64, 1, 4, 0x400000, 0, 0x200, 0x200);
  segment(120, 1, 5, text_address, 0x200, text_file, text_mem);
  segment(176, 4, 5, text_address, 0x200, 4, 4);
  return bytes;
}

Bytes elf32be() {
  Bytes bytes(0x84);
  text(bytes, 0, "\x7f" "ELF");
  bytes[4] = 1; bytes[5] = 2; bytes[6] = 1;
  const auto be = [&](std::size_t offset, u64 value, unsigned width = 4) {
    put(bytes, offset, value, width, true);
  };
  be(16, 3, 2); // ET_DYN
  be(18, 20, 2);
  be(20, 1);
  be(28, 52);
  be(40, 52, 2);
  be(42, 32, 2);
  be(44, 1, 2);
  be(52, 1);
  be(56, 0x80);
  be(60, 0xfffffffc);
  be(68, 4);
  be(72, 4);
  be(76, 5);
  be(80, 1);
  return bytes;
}

Bytes pe(bool wide = true) {
  Bytes bytes(0x600);
  const unsigned opt_size = wide ? 112 : 96;
  text(bytes, 0, "MZ");
  put(bytes, 0x3c, 0x80);
  text(bytes, 0x80, "PE");
  put(bytes, 0x84, wide ? 0x8664 : 0x014c, 2);
  put(bytes, 0x86, 3, 2);
  put(bytes, 0x94, opt_size, 2);
  put(bytes, 0x96, 2, 2);
  constexpr std::size_t opt = 0x98;
  put(bytes, opt, wide ? 0x20b : 0x10b, 2);
  put(bytes, opt + 16, 0x1000);
  if (wide) put(bytes, opt + 24, 0x140000000, 8);
  else put(bytes, opt + 28, 0x400000);
  put(bytes, opt + 32, 0x1000);
  put(bytes, opt + 36, 0x200);
  put(bytes, opt + 56, 0x4000);
  put(bytes, opt + 60, 0x200);
  const auto section = [&](std::size_t index, std::string_view name, unsigned rva,
                           unsigned vm_size, unsigned file_size, unsigned offset,
                           unsigned flags) {
    const auto o = opt + opt_size + index * 40;
    text(bytes, o, name);
    put(bytes, o + 8, vm_size);
    put(bytes, o + 12, rva);
    put(bytes, o + 16, file_size);
    put(bytes, o + 20, offset);
    put(bytes, o + 36, flags);
  };
  section(0, ".text", 0x1000, 0x10, 0x200, 0x200, 0x60000020);
  section(1, ".tail", 0x2000, 0x240, 0x200, 0x400, 0x60000020);
  section(2, ".data", 0x3000, 0x200, 0, 0, 0x400000a0); // code bit != execute bit
  return bytes;
}

Bytes macho() {
  Bytes bytes(0x304);
  put(bytes, 0, 0xfeedfacf);
  put(bytes, 4, 0x01000007);
  put(bytes, 8, 3);
  put(bytes, 12, 2);
  put(bytes, 16, 3);
  put(bytes, 20, 3 * 72);
  const auto segment = [&](std::size_t o, std::string_view name, u64 address,
                           u64 file_off, u64 file, u64 mem, unsigned prot) {
    put(bytes, o, 0x19);
    put(bytes, o + 4, 72);
    text(bytes, o + 8, name);
    put(bytes, o + 24, address, 8);
    put(bytes, o + 32, mem, 8);
    put(bytes, o + 40, file_off, 8);
    put(bytes, o + 48, file, 8);
    put(bytes, o + 56, prot);
    put(bytes, o + 60, prot);
  };
  segment(32, "__PAGEZERO", 0, 0, 0, 0x100000000, 0);
  segment(32 + 72, "__TEXT", 0x100000000, 0, 0x300, 0x400, 5);
  segment(32 + 144, "__D\x1b" "TA", 0x100001000, 0x300, 4, 8, 3);
  return bytes;
}

Bytes macho32be() {
  Bytes bytes(0x100);
  const auto be = [&](std::size_t offset, u64 value) { put(bytes, offset, value, 4, true); };
  be(0, 0xfeedface);
  be(4, 18);
  be(12, 2);
  be(16, 1);
  be(20, 56);
  constexpr std::size_t seg = 28;
  be(seg, 1);
  be(seg + 4, 56);
  text(bytes, seg + 8, "__TEXT");
  be(seg + 24, 0x1000);
  be(seg + 28, 0x200);
  be(seg + 36, 0x100);
  be(seg + 40, 5);
  be(seg + 44, 5);
  return bytes;
}

template <class T>
T success(Result<T> result) {
  if (const auto* e = std::get_if<Error>(&result)) {
    std::cerr << e->message << '\n';
    std::exit(1);
  }
  return std::get<T>(std::move(result));
}

template <class T>
bool has_error(const Result<T>& result, ErrorCode expected) {
  const auto* error = std::get_if<Error>(&result);
  return error && error->code == expected;
}

void check_miss(const Image& image, u64 pc, S status, std::optional<std::size_t> index,
                std::optional<u64> runtime_base = {}) {
  const auto a = image.attribute(pc, runtime_base);
  CHECK(a.status == status && !a.file_offset && a.range_index == index);
  CHECK(a.linked_pc.has_value() == (status != S::AddressOutOfRange));
}

void image_tests() {
  auto bytes = elf64();
  const auto elf = success(Image::from_bytes(bytes));
  bytes.clear();
  CHECK(elf.format() == Format::ELF && elf.address_bits() == 64);
  CHECK(elf.preferred_base() == 0x400000 && elf.ranges().size() == 2);
  const auto a = elf.attribute(0x402000);
  CHECK(a.status == S::Attributed && a.linked_pc == 0x402000 && a.range_index == 1);
  CHECK(a.file_offset == 0x200);
  CHECK(elf.attribute(0x402003).file_offset == 0x203);
  CHECK(elf.attribute(0x50902003, 0x50900000).file_offset == 0x203);
  CHECK(elf.attribute(0x30002003, 0x30000000).file_offset == 0x203);
  check_miss(elf, 0x402004, S::NotFileBacked, 1);
  check_miss(elf, 0x402007, S::NotFileBacked, 1);
  check_miss(elf, 0x402008, S::OutsideRanges, {});
  check_miss(elf, 0x400000, S::NonExecutable, 0);
  check_miss(elf, 0x5000, S::AddressOutOfRange, {}, 0x5001);
  check_miss(elf, max_u64, S::AddressOutOfRange, {}, 0);

  constexpr std::array samples{Sample{0x402000, 2}, Sample{0x402001}, Sample{0x402004, 4},
                               Sample{0x403000, 8}, Sample{0x400100, 16}, Sample{0, 32}};
  const auto p = success(elf.profile(samples, 0x400000));
  CHECK(p.total_weight == 63 && p.attributed_weight == 3);
  CHECK(p.weight_by_range == (std::vector<u64>{0, 3}));
  CHECK(p.not_file_backed_weight == 4 && p.outside_ranges_weight == 8);
  CHECK(p.non_executable_weight == 16 && p.address_out_of_range_weight == 32);
  CHECK(success(elf.profile({})).total_weight == 0);
  constexpr std::array max_weight{Sample{0x402000, max_u64}};
  CHECK(success(elf.profile(max_weight)).attributed_weight == max_u64);
  constexpr std::array overflow{Sample{0x402000, max_u64}, Sample{0x499999, 1}};
  CHECK(has_error(elf.profile(overflow), ErrorCode::Overflow));
  constexpr std::array zero{Sample{0x402000, 0}};
  CHECK(has_error(elf.profile(zero), ErrorCode::InvalidInput));

  const auto elf_top = success(Image::from_bytes(elf64(max_u64 - 3, 4, 4)));
  CHECK(elf_top.attribute(max_u64).file_offset == 0x203);
  CHECK(has_error(Image::from_bytes(elf64(max_u64 - 3, 5, 4)), ErrorCode::Malformed));
  CHECK(has_error(Image::from_bytes(elf64(0x400100)), ErrorCode::Unsupported));
  const auto adjacent = success(Image::from_bytes(elf64(0x400200)));
  CHECK(adjacent.attribute(0x400200).file_offset == 0x200);
  const auto elf_32 = success(Image::from_bytes(elf32be()));
  CHECK(elf_32.address_bits() == 32 && elf_32.preferred_base() == 0xfffffffc);
  CHECK(elf_32.attribute(0xffffffff).file_offset == 0x83);
  CHECK(elf_32.attribute(0x1003, 0x1000).file_offset == 0x83);
  check_miss(elf_32, 0x1004, S::AddressOutOfRange, {}, 0x1000);
  check_miss(elf_32, 0x100000000, S::AddressOutOfRange, {});

  const std::array points{PlanPoint{"whole", 0x80002000, 4}, PlanPoint{"last.byte", 0x80002003}};
  const auto plan = success(elf.verification_plan(points, 0x80000000));
  CHECK(plan.size() == 2 && plan[0].name == "whole" && plan[0].size == 4);
  CHECK(plan[0].linked_pc == 0x402000 && plan[0].file_offset == 0x200 && plan[0].range_index == 1);
  CHECK(plan[1].name == "last.byte" && plan[1].file_offset == 0x203 && plan[1].size == 1);
  CHECK(success(elf.verification_plan({})).empty());
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"tail", 0x402004}}),
                  ErrorCode::InvalidInput));
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"nonexec", 0x400000}}),
                  ErrorCode::InvalidInput));
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"cross", 0x402003, 2}}),
                  ErrorCode::InvalidInput));
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"zero", 0x402000, 0}}),
                  ErrorCode::InvalidInput));
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"\n", 0x402000}}),
                  ErrorCode::InvalidInput));
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"dup", 0x402000}, PlanPoint{"dup", 0x402001}}),
                  ErrorCode::InvalidInput));
  CHECK(has_error(elf.verification_plan(std::array{PlanPoint{"wrap", max_u64, 2}}, max_u64 - 0x2000),
                  ErrorCode::Overflow));
  CHECK(has_error(elf_32.verification_plan(std::array{PlanPoint{"edge", 0xffffffff, 2}}),
                  ErrorCode::Overflow));
  CHECK(has_error(elf_32.verification_plan(std::array{PlanPoint{"edge", 0x1003, 2}}, 0x1000),
                  ErrorCode::Overflow));
  CHECK(success(elf_top.verification_plan(std::array{PlanPoint{"top", max_u64}}))[0].file_offset == 0x203);
  CHECK(has_error(elf.verification_plan(std::vector<PlanPoint>(max_plan_points + 1)), ErrorCode::Limit));

  const auto windows = success(Image::from_bytes(pe()));
  CHECK(windows.format() == Format::PE && windows.preferred_base() == 0x140000000);
  CHECK(windows.attribute(0x14000100f).file_offset == 0x20f);
  check_miss(windows, 0x140001010, S::OutsideRanges, {});
  CHECK(windows.attribute(0x1400021ff).file_offset == 0x5ff);
  check_miss(windows, 0x140002200, S::NotFileBacked, 1);
  check_miss(windows, 0x140003000, S::NonExecutable, 2);
  check_miss(windows, 0x140000010, S::OutsideRanges, {});
  auto malformed_pe = pe();
  put(malformed_pe, 0x98 + 112 + 20, 0); // maps headers, not payload
  CHECK(has_error(Image::from_bytes(malformed_pe), ErrorCode::Malformed));
  const auto win32 = success(Image::from_bytes(pe(false)));
  CHECK(win32.address_bits() == 32 && win32.preferred_base() == 0x400000);
  CHECK(win32.attribute(0x501003, 0x500000).file_offset == 0x203);
  malformed_pe = pe(false);
  put(malformed_pe, 0x98 + 28, 0xffffd000); // SizeOfImage crosses 32-bit limit.
  CHECK(has_error(Image::from_bytes(malformed_pe), ErrorCode::Malformed));

  const auto mac = success(Image::from_bytes(macho()));
  CHECK(mac.format() == Format::MachO && mac.preferred_base() == 0x100000000);
  CHECK(mac.ranges().size() == 3);
  CHECK(mac.attribute(0x1000002ff).file_offset == 0x2ff);
  CHECK(mac.attribute(0x130000100, 0x130000000).file_offset == 0x100);
  check_miss(mac, 0, S::NonExecutable, 0);
  check_miss(mac, 0x100000300, S::NotFileBacked, 1);
  check_miss(mac, 0x100000400, S::OutsideRanges, {});
  check_miss(mac, 0x100001000, S::NonExecutable, 2);
  auto highvm = macho();
  put(highvm, 32 + 72 + 68, 1);
  CHECK(has_error(Image::from_bytes(highvm), ErrorCode::Unsupported));
  const auto mac32 = success(Image::from_bytes(macho32be()));
  CHECK(mac32.address_bits() == 32 && mac32.preferred_base() == 0x1000);
  CHECK(mac32.attribute(0x10ff).file_offset == 0xff);
  check_miss(mac32, 0x1100, S::NotFileBacked, 0);

  for (auto fixture : {elf64(), elf32be(), pe(), pe(false), macho(), macho32be()}) {
    for (std::size_t length : {std::size_t{4}, std::size_t{32}, fixture.size() - 1})
      CHECK(has_error(Image::from_bytes({fixture.data(), length}), ErrorCode::Malformed));
  }
  auto reloc = elf64();
  put(reloc, 16, 1, 2);
  CHECK(has_error(Image::from_bytes(reloc), ErrorCode::Unsupported));
  auto universal = macho();
  put(universal, 0, 0xbebafeca);
  CHECK(has_error(Image::from_bytes(universal), ErrorCode::Unsupported));
  CHECK(has_error(Image::from_bytes(Bytes{0, 1, 2, 3}), ErrorCode::Unsupported));
}

void graph_tests() {
  const std::vector<Block> model{{7, {7}}, {3, {2}}, {0, {1, 2}}, {2, {3, 5}},
                                 {1, {}}, {5, {5}}, {6, {0}}};
  const auto summary = success(summarize_graph(0, model));
  CHECK(summary.reachable == (std::vector<BlockId>{0, 1, 2, 3, 5}));
  CHECK(summary.unreachable == (std::vector<BlockId>{6, 7}));
  CHECK(summary.reachable_cycles == (std::vector<std::vector<BlockId>>{{2, 3}, {5}}));
  CHECK(has_error(summarize_graph(0, {}), ErrorCode::InvalidInput));
  CHECK(has_error(summarize_graph(0, std::array{Block{0, {1}}}), ErrorCode::InvalidInput));
  CHECK(has_error(summarize_graph(0, std::array{Block{0, {0, 0}}}), ErrorCode::InvalidInput));
  CHECK(has_error(summarize_graph(0, std::array{Block{0, {0, 1, 2}}}), ErrorCode::Unsupported));
  CHECK(has_error(summarize_graph(0, std::array{Block{0, {}}, Block{0, {}}}), ErrorCode::InvalidInput));
  constexpr auto reserved = std::numeric_limits<BlockId>::max();
  CHECK(has_error(summarize_graph(reserved, std::array{Block{reserved, {}}}), ErrorCode::InvalidInput));
  std::vector<Block> chain;
  for (BlockId id = 0; id < max_model_blocks; ++id)
    chain.push_back({id, {static_cast<BlockId>((id + 1) % max_model_blocks)}});
  CHECK(success(summarize_graph(0, chain)).reachable_cycles.front().size() == max_model_blocks);
  chain.push_back({static_cast<BlockId>(max_model_blocks), {}});
  CHECK(has_error(summarize_graph(0, chain), ErrorCode::Limit));
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    const std::filesystem::path path(argv[1]);
    const auto write = [&](const char* name, const Bytes& bytes) {
      std::ofstream file(path / name, std::ios::binary);
      CHECK(file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };
    write("image.elf", elf64());
    write("image.exe", pe());
    write("image.macho", macho());
    return 0;
  }
  CHECK(argc == 1);
  image_tests();
  graph_tests();
}
