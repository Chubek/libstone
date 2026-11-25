#include "wasmutils/WASM-Module.hpp"
#include "../../tests/test.hpp"

#include <initializer_list>
#include <utility>

using limestone::wasmutils::Error;
namespace core = limestone::wasmutils::core;

namespace {

using Bytes = std::vector<std::uint8_t>;
using core::Type;
using core::SegmentMode;
using core::ExternalKind;

const Bytes header{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};

Bytes binary(std::initializer_list<std::pair<std::uint8_t, Bytes>> sections) {
  Bytes result = header;
  for (const auto& [id, payload] : sections) {
    CHECK(payload.size() < 128);
    result.push_back(id);
    result.push_back(static_cast<std::uint8_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
  }
  return result;
}

template<class Run>
void rejects(Run run, std::optional<std::size_t> offset = {}) {
  try {
    run();
  } catch (const Error& error) {
    if (offset)
      CHECK(error.offset == *offset);
    return;
  }
  CHECK(false && "expected a Core 2.0 module diagnostic");
}

void bad_binary(std::initializer_list<std::pair<std::uint8_t, Bytes>> sections,
                std::optional<std::size_t> offset = {}) {
  rejects([&] { core::decode_module(binary(sections)); }, offset);
}

void test_sections() {
  const auto module = core::decode_module(binary({
    {0, {2, 0xcf, 0x80, 0x80, 0xff}},                  // π, opaque payload
    {1, {3, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 0, 0,
          0x60, 3, 0x7d, 0x7b, 0x6f, 3, 0x7e, 0x7c, 0x70}},
    {2, {4,
         1, 'm', 1, 'f', 0, 0,                         // function, type 0
         1, 'm', 1, 't', 1, 0x6f, 1, 2, 7,            // table, externref, 2..7
         1, 'm', 1, 'm', 2, 1, 1, 2,                  // memory, 1..2
         1, 'm', 1, 'g', 3, 0x7e, 0}},                // immutable i64 global
    {3, {2, 1, 0}},
    {4, {1, 0x70, 0, 2}},
    {6, {2,
         0x7e, 1, 0x23, 0, 0x0b,                      // mutable i64 = global.get 0
         0x7c, 0, 0x44, 0x0b, 0, 0, 0, 0, 0, 0xf8, 0x7f, 0x0b}},
    {7, {4, 1, 'f', 0, 2, 1, 't', 1, 1, 1, 'm', 2, 0, 1, 'g', 3, 1}},
    {8, {1}},
    {0, {1, 0, 0x80}},                                // embedded NUL in the name
    {9, {1, 2, 1, 0x41, 0, 0x0b, 0, 1, 2}},          // active table 1, function 2
    {12, {2}},
    {10, {2, 6, 0, 0x02, 0x40, 0x01, 0x0b, 0x0b,
             8, 2, 1, 0x7e, 2, 0x7d, 0x20, 0, 0x0b}},
    {11, {2, 0, 0x41, 0, 0x0b, 3, 0, 0xff, 0x0b, 1, 2, 0xfe, 0x80}},
    {0, {0}}                                           // empty name, empty payload
  }));
  CHECK(module.types == std::vector<core::FunctionType>{
    {{Type::i32}, {Type::i32}}, {{}, {}},
    {{Type::f32, Type::v128, Type::externref}, {Type::i64, Type::f64, Type::funcref}}});
  CHECK(module.imports.size() == 4);
  CHECK(module.imports[0].module == "m" && module.imports[0].name == "f");
  CHECK(std::get<std::uint32_t>(module.imports[0].desc) == 0);
  CHECK(std::get<core::TableType>(module.imports[1].desc) == core::TableType{Type::externref, {2, 7}});
  CHECK(std::get<core::MemoryType>(module.imports[2].desc) == core::MemoryType{{1, 2}});
  CHECK(std::get<core::GlobalType>(module.imports[3].desc) == core::GlobalType{Type::i64, false});
  CHECK(module.functions.size() == 2 && module.functions[0].type == 1 && module.functions[1].type == 0);
  CHECK(module.functions[0].body.instructions.size() == 4);
  CHECK(module.functions[0].body.instructions[0].opcode == core::Opcode{0x02});
  CHECK(module.functions[1].body.locals == std::vector<core::LocalRun>{{1, Type::i64}, {2, Type::f32}});
  CHECK(module.functions[1].body.instructions[0].offset == module.functions[1].body.offset + 5);
  CHECK(module.tables == std::vector<core::TableType>{{Type::funcref, {2, {}}}});
  CHECK(module.memories.empty());
  CHECK(module.globals.size() == 2 && module.globals[0].type == core::GlobalType{Type::i64, true});
  CHECK(module.globals[1].init[0].opcode == core::Opcode{0x44});
  CHECK(std::get<std::uint64_t>(module.globals[1].init[0].immediate) == 0x7ff800000000000bULL);
  CHECK(module.exports.size() == 4 && module.exports[3].kind == ExternalKind::global);
  CHECK(module.exports[3].name == "g" && module.exports[3].index == 1);
  CHECK(module.start == 1);
  CHECK(module.elements.size() == 1 && module.elements[0].mode == SegmentMode::active);
  CHECK(module.elements[0].table == 1 && module.elements[0].type == Type::funcref);
  CHECK(module.elements[0].init.size() == 1);
  CHECK(module.elements[0].init[0][0].opcode == core::Opcode{0xd2});
  CHECK(std::get<std::uint32_t>(module.elements[0].init[0][0].immediate) == 2);
  CHECK(module.data_count == 2 && module.data.size() == 2);
  CHECK(module.data[0].mode == SegmentMode::active && module.data[0].memory == 0);
  CHECK(module.data[0].bytes == Bytes{0, 0xff, 0x0b});
  CHECK(module.data[1].mode == SegmentMode::passive && module.data[1].offset.empty());
  CHECK(module.data[1].bytes == Bytes{0xfe, 0x80});
  CHECK(module.custom.size() == 3 && module.custom[0].name == "\xcf\x80");
  CHECK(module.custom[0].bytes == Bytes{0x80, 0xff});
  CHECK(module.custom[1].name == std::string(1, '\0') && module.custom[1].bytes == Bytes{0x80});
  CHECK(module.custom[2].name.empty() && module.custom[2].bytes.empty());

  const auto canonical = core::encode_module(module);
  const auto reparsed = core::decode_module(canonical);
  CHECK(reparsed.custom.size() == 3 && reparsed.custom[0].bytes == module.custom[0].bytes);
  CHECK(reparsed.custom[1].name == module.custom[1].name);
  CHECK(core::encode_module(reparsed) == canonical);

  const auto memories = binary({{5, {1, 0, 1}}, {11, {1, 2, 0, 0x41, 0, 0x0b, 0}}});
  const auto memory_module = core::decode_module(memories);
  CHECK(memory_module.memories == std::vector<core::MemoryType>{{{1, {}}}});
  CHECK(memory_module.data[0].memory == 0 && memory_module.data[0].mode == SegmentMode::active);
  // Explicit index 0 canonicalizes to the implicit-index form.
  CHECK(core::encode_module(memory_module) == binary({{5, {1, 0, 1}}, {11, {1, 0, 0x41, 0, 0x0b, 0}}}));
}

void test_segments_and_leb() {
  const auto bytes = binary({
    {9, {8,
      0, 0x41, 0, 0x0b, 1, 0,                             // implicit active, legacy
      0x81, 0, 0, 1, 1,                                  // passive, legacy, padded flags
      2, 2, 0x41, 0, 0x0b, 0, 1, 2,                       // explicit active, legacy
      3, 0, 1, 3,                                        // declarative, legacy
      4, 0x41, 0, 0x0b, 1, 0xd0, 0x70, 0x0b,               // implicit active, expression
      5, 0x6f, 2, 0xd0, 0x6f, 0x0b, 0xd0, 0x6f, 0x0b,      // passive, two expressions
      6, 0, 0x41, 0, 0x0b, 0x6f, 1, 0xd0, 0x6f, 0x0b,      // explicit table 0, externref
      7, 0x70, 1, 0xd0, 0x70, 0x0b}},                     // declarative, expression
    {11, {1, 2, 3, 0x41, 0, 0x0b, 1, 0x0b}}
  });
  const auto module = core::decode_module(bytes);
  const auto& e = module.elements;
  CHECK(e.size() == 8);
  CHECK(e[0].mode == SegmentMode::active && e[0].table == 0 && e[0].type == Type::funcref);
  CHECK(e[1].mode == SegmentMode::passive && e[1].offset.empty());
  CHECK(e[2].mode == SegmentMode::active && e[2].table == 2);
  CHECK(e[3].mode == SegmentMode::declarative && e[3].offset.empty());
  CHECK(e[4].mode == SegmentMode::active && e[4].table == 0 && e[4].type == Type::funcref);
  CHECK(e[5].mode == SegmentMode::passive && e[5].type == Type::externref && e[5].init.size() == 2);
  CHECK(e[5].init[1][0].offset == e[5].init[0][0].offset + 3);
  CHECK(e[6].mode == SegmentMode::active && e[6].table == 0 && e[6].type == Type::externref);
  CHECK(e[7].mode == SegmentMode::declarative && e[7].type == Type::funcref);
  CHECK(module.data[0].memory == 3 && module.data[0].bytes == Bytes{0x0b});
  auto canonical = bytes;
  canonical.erase(canonical.begin() + 18); // second byte of padded flags
  --canonical[9];
  canonical[17] = 1;
  CHECK(core::encode_module(module) == canonical);

  Bytes padded = header;
  const Bytes payload{1, 0x85, 0, 0x81, 0, 0x60, 0, 0, 8, 5, 0xff, 0xff, 0xff, 0xff, 0x0f};
  padded.insert(padded.end(), payload.begin(), payload.end());
  const auto padded_module = core::decode_module(padded);
  CHECK(padded_module.types.size() == 1 && padded_module.start == UINT32_MAX);
  CHECK(core::encode_module(padded_module) == binary({
    {1, {1, 0x60, 0, 0}}, {8, {0xff, 0xff, 0xff, 0xff, 0x0f}}
  }));
  CHECK(core::encode_module(core::decode_module(binary({{1, {0}}, {3, {0}}, {10, {0}}}))) == header);

  core::Module larger;
  larger.custom.push_back({std::string(128, 'a'), Bytes(130, 0xff)});
  const auto larger_bytes = core::encode_module(larger);
  // The section payload is 260 bytes, then the 128-byte name.
  CHECK(Bytes(larger_bytes.begin() + 8, larger_bytes.begin() + 13) == Bytes{0, 0x84, 2, 0x80, 1});
  CHECK(core::encode_module(core::decode_module(larger_bytes)) == larger_bytes);

  const auto max_locals = binary({
    {3, {1, 0}}, {10, {1, 8, 1, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x7f, 0x0b}}
  });
  const auto locals_module = core::decode_module(max_locals);
  CHECK(locals_module.functions[0].body.locals == std::vector<core::LocalRun>{{UINT32_MAX, Type::i32}});
  CHECK(core::encode_module(locals_module) == max_locals);
}

void test_offsets_and_data_count() {
  const auto values = binary({
    {6, {1, 0x7f, 0, 0x41, 0x8b, 0, 0x0b}},
    {9, {1, 5, 0x6f, 1, 0xd0, 0x6f, 0x0b}}
  });
  const auto parsed = core::decode_module(values);
  CHECK(parsed.globals[0].init[0].offset == 13 && parsed.globals[0].init[1].offset == 16);
  CHECK(parsed.elements[0].init[0][0].offset == 23);
  CHECK(parsed.elements[0].init[0][1].offset == 25);
  const auto no_count = binary({{3, {1, 0}}, {10, {1, 5, 0, 0xfc, 9, 0, 0x0b}}, {11, {1, 1, 0}}});
  rejects([&] { core::decode_module(no_count); }, 17);
  const auto has_count = binary({
    {3, {1, 0}}, {12, {1}}, {10, {1, 5, 0, 0xfc, 9, 0, 0x0b}}, {11, {1, 1, 0}}
  });
  auto module = core::decode_module(has_count);
  CHECK(module.data_count == 1 && core::encode_module(module) == has_count);
  module.data_count.reset();
  rejects([&] { core::encode_module(module); }, 20);
  CHECK(core::encode_module(core::decode_module(binary({{12, {0}}}))) == binary({{12, {0}}}));
}

void test_bad_binaries() {
  rejects([] { core::decode_module(Bytes{0, 0x61, 0x73, 0x6d}); }, 0);
  rejects([] { core::decode_module(Bytes{0, 0x61, 0x73, 0x6d, 2, 0, 0, 0}); }, 0);
  auto truncated = header;
  truncated.insert(truncated.end(), {1, 2, 0});
  rejects([&] { core::decode_module(truncated); });
  auto overlong = header;
  overlong.insert(overlong.end(), {1, 0x80, 0x80, 0x80, 0x80, 0x80, 0});
  rejects([&] { core::decode_module(overlong); }, 9);

  bad_binary({{13, {}}}, 8);
  bad_binary({{1, {0}}, {1, {0}}}, 11);
  bad_binary({{7, {0}}, {0, {0}}, {6, {0}}}, 14);
  bad_binary({{10, {0}}, {12, {0}}}, 11);
  bad_binary({{3, {1, 0}}});
  bad_binary({{10, {1, 2, 0, 0x0b}}});
  bad_binary({{3, {1, 0}}, {10, {0}}});
  bad_binary({{3, {1, 0}}, {10, {1, 3, 0, 0x0b, 0x01}}}, 18);
  bad_binary({{3, {1, 0}}, {10, {1, 3, 0, 0x02, 0x40}}});
  bad_binary({{3, {1, 0}}, {10, {1, 3, 1, 1, 0x40}}});
  bad_binary({{3, {1, 0}}, {10, {1, 10, 2, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x7f, 1, 0x7f, 0x0b}}});
  bad_binary({{12, {1}}}, 10);
  bad_binary({{12, {0}}, {11, {1, 1, 0}}}, 10);
  bad_binary({{1, {0xff, 0xff, 0xff, 0xff, 0x0f}}});
  bad_binary({{1, {1, 0x5f, 0, 0}}});
  bad_binary({{1, {1, 0x60, 1, 0x40, 0}}});
  bad_binary({{1, {0, 1}}}, 11);
  bad_binary({{2, {1, 0, 0, 4, 0}}});
  bad_binary({{4, {1, 0x7f, 0, 0}}});
  bad_binary({{5, {1, 3, 1, 2}}}); // Shared memory is outside Core 2.0.
  bad_binary({{6, {1, 0x7f, 2, 0x0b}}});
  bad_binary({{6, {1, 0x7f, 0, 0xfd, 0x0c}}, {7, {0}}}); // Do not read the next section.
  bad_binary({{7, {1, 0, 4, 0}}});
  bad_binary({{8, {0x80, 0x80, 0x80, 0x80, 0x10}}});
  bad_binary({{9, {1, 8, 0, 0}}});
  bad_binary({{9, {1, 3, 1, 0}}});
  bad_binary({{9, {1, 5, 0x7f, 0}}});
  bad_binary({{11, {1, 3, 0}}});
  bad_binary({{11, {1, 1, 4, 0}}});
  bad_binary({{0, {}}});
  bad_binary({{0, {2, 0xc0, 0x80}}});
  bad_binary({{0, {3, 0xe0, 0x80, 0x80}}});
  bad_binary({{0, {3, 0xed, 0xa0, 0x80}}});
  bad_binary({{0, {4, 0xf4, 0x90, 0x80, 0x80}}});
  bad_binary({{0, {1, 0xe2, 0x80, 0xa8}}});
}

void test_invalid_models() {
  core::Module module;
  module.data_count = 1;
  rejects([&] { core::encode_module(module); });
  module = {};
  module.custom.push_back({"\xff", {}});
  rejects([&] { core::encode_module(module); });
  module = {};
  module.tables.push_back({Type::i32, {0, {}}});
  rejects([&] { core::encode_module(module); });
  module = {};
  module.exports.push_back({"x", static_cast<ExternalKind>(4), 0});
  rejects([&] { core::encode_module(module); });
  module = {};
  module.elements.push_back({SegmentMode::passive, 1, {}, Type::funcref, {}});
  rejects([&] { core::encode_module(module); });
  module = {};
  module.data.push_back({SegmentMode::declarative, 0, {}, {}});
  rejects([&] { core::encode_module(module); });
  module = {};
  module.globals.push_back({{Type::i64, false}, {}});
  rejects([&] { core::encode_module(module); });
  module = {};
  module.functions.push_back({0, {0, {{UINT32_MAX, Type::i64}, {1, Type::i64}}, {{{0x0b}, {}}}}});
  rejects([&] { core::encode_module(module); });
}

} // namespace

int main() {
  test_sections();
  test_segments_and_leb();
  test_offsets_and_data_count();
  test_bad_binaries();
  test_invalid_models();
}
