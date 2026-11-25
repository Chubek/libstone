#include "wasmutils/WASM-Link.hpp"
#include "wasmutils/WASM-Validate.hpp"
#include "../../tests/test.hpp"

#include <array>

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;

namespace {

using core::ExternalKind;
using core::IndexPair;
using core::Instruction;
using core::Module;
using core::Program;
using core::SegmentMode;
using core::Type;
using Index = std::uint32_t;

constexpr auto i32 = Type::i32;
constexpr auto i64 = Type::i64;
constexpr auto funcref = Type::funcref;
constexpr auto func = ExternalKind::function;
constexpr auto table = ExternalKind::table;
constexpr auto memory = ExternalKind::memory;
constexpr auto global = ExternalKind::global;
constexpr auto active = SegmentMode::active;
constexpr auto passive = SegmentMode::passive;

Instruction op(std::uint8_t byte, core::Immediate immediate = {}) {
  return {{byte}, std::move(immediate), 0};
}

Instruction bulk(Index extension, core::Immediate immediate = {}) {
  return {{0xfc, extension}, std::move(immediate), 0};
}

Program constant(Index value) { return {op(0x41, value), op(0x0b)}; }
Program global_get(Index index) { return {op(0x23, index), op(0x0b)}; }
Program ref_func(Index index) { return {op(0xd2, index), op(0x0b)}; }

Index index_of(const Instruction& instruction) {
  return std::get<Index>(instruction.immediate);
}

Module link(std::initializer_list<Module> inputs) {
  return core::link_modules({inputs.begin(), inputs.size()});
}

template<class Run>
void rejects(std::string_view message, Run run) {
  try {
    run();
  } catch (const wasm::Error& error) {
    CHECK(std::string_view(error.what()).find(message) != std::string_view::npos);
    return;
  }
  CHECK(false && "expected a link diagnostic");
}

void calls_and_types() {
  Module provider;
  provider.types = {{{}, {i64}}, {{}, {i32}}};
  provider.functions = {{1, {0, {}, constant(42)}}};
  provider.exports = {{"answer", func, 0}};
  provider.custom = {{"name", {1, 2}}};

  Module caller;
  caller.types = {{{}, {i32}}, {}, {{}, {i32}}};
  caller.imports = {{"provider", "answer", Index{2}}, {"host", "log", Index{1}}};
  caller.functions = {{0, {0, {{1, i32}},
      {op(0x10, Index{1}), op(0x02, core::BlockType{Index{2}}),
       op(0x10, Index{0}), op(0x21, Index{0}), op(0x20, Index{0}), op(0x0c, Index{0}),
       op(0x0b), op(0x0b)}}}};
  caller.exports = {{"run", func, 2}};

  const auto result = link({provider, caller});
  CHECK(result.types.size() == 3);
  CHECK(result.imports.size() == 1);
  CHECK(result.imports[0].module == "host");
  CHECK(result.imports[0].name == "log");
  CHECK(std::get<Index>(result.imports[0].desc) == 2);
  CHECK(result.exports[0].index == 1);
  CHECK(result.exports[1].index == 2);
  CHECK(result.functions[0].type == 1);
  CHECK(result.functions[1].type == 1);
  const auto& code = result.functions[1].body.instructions;
  CHECK(index_of(code[0]) == 0);
  CHECK(std::get<Index>(std::get<core::BlockType>(code[1].immediate).value) == 1);
  CHECK(index_of(code[2]) == 1);
  CHECK(index_of(code[3]) == 0);
  CHECK(index_of(code[4]) == 0);
  CHECK(index_of(code[5]) == 0);
  CHECK(result.custom.empty());
  CHECK(index_of(caller.functions[0].body.instructions[0]) == 1);

  const auto bytes = core::encode_module(result);
  const auto reparsed = core::decode_module(bytes);
  core::validate_module(reparsed);
  CHECK(core::encode_module(reparsed) == bytes);
  CHECK(core::encode_module(link({provider, caller})) == bytes);
}

void tables_globals_memory_and_segments() {
  Module provider;
  provider.types = {{}};
  provider.functions = {{0, {0, {}, {op(0x0b)}}}, {0, {0, {}, {op(0x0b)}}}};
  provider.tables = {{funcref, {1, 4}}, {funcref, {8, 16}}};
  provider.memories = {{{2, 3}}};
  provider.globals = {{{i32, false}, constant(6)}, {{i32, true}, constant(9)}};
  provider.exports = {{"callee", func, 1}, {"slots", table, 1}, {"ram", memory, 0},
                      {"base", global, 0}, {"count", global, 1}};
  provider.elements = {{passive, 0, {}, funcref, {ref_func(1)}}};
  provider.data = {{passive, 0, {}, {123}}};

  Module caller;
  caller.types = {{}};
  caller.imports = {{"provider", "base", core::GlobalType{i32, false}},
                    {"provider", "count", core::GlobalType{i32, true}},
                    {"provider", "ram", core::MemoryType{{1, 4}}},
                    {"provider", "slots", core::TableType{funcref, {2, 20}}},
                    {"provider", "callee", Index{0}},
                    {"host", "global", core::GlobalType{i32, false}}};
  caller.tables = {{funcref, {8, 16}}};
  caller.globals = {{{i32, false}, global_get(0)}};
  caller.exports = {{"copy_of_base", global, 3}, {"private_table", table, 1}};
  caller.elements = {{active, 1, global_get(0), funcref, {ref_func(0), ref_func(1)}},
                     {passive, 0, {}, funcref, {ref_func(1)}}};
  caller.data = {{active, 0, global_get(0), {45}}, {passive, 0, {}, {67}}};
  caller.data_count = 2;
  caller.functions = {{0, {0, {}, {
      op(0x00), // Keep index-use coverage compact with a polymorphic stack.
      bulk(8, Index{1}), bulk(9, Index{1}), bulk(10), bulk(11),
      op(0x3f), op(0x1a), op(0x40), op(0x1a),
      bulk(12, IndexPair{1, 1}), bulk(13, Index{1}), bulk(14, IndexPair{1, 0}),
      bulk(15, Index{1}), op(0x1a), bulk(16, Index{0}), op(0x1a), bulk(17, Index{0}),
      op(0x25, Index{1}), op(0x1a), op(0x26, Index{0}),
      op(0x10, Index{0}), op(0xd2, Index{1}), op(0x1a), op(0x11, IndexPair{0, 1}),
      op(0x23, Index{1}), op(0x1a), op(0x24, Index{1}),
      op(0x23, Index{2}), op(0x1a), op(0x28, core::MemoryArg{2, 127}), op(0x1a), op(0x0b)}}}};

  const auto result = link({provider, caller});
  CHECK(result.imports.size() == 1 && result.imports[0].name == "global");
  CHECK(result.memories.size() == 1);
  CHECK(result.tables.size() == 3);
  CHECK(result.globals[2].init == constant(6));
  CHECK(result.exports[5].index == 3);
  CHECK(result.exports[6].index == 2);
  CHECK(result.elements[1].table == 2);
  CHECK(result.elements[1].offset == constant(6));
  CHECK(result.elements[1].init == std::vector<Program>{ref_func(1), ref_func(2)});
  CHECK(result.elements[2].init == std::vector<Program>{ref_func(2)});
  CHECK(result.data[1].memory == 0);
  CHECK(result.data[1].offset == constant(6));
  CHECK(result.data_count == 3);
  const auto& code = result.functions[2].body.instructions;
  CHECK(index_of(code[1]) == 2);
  CHECK(index_of(code[2]) == 2);
  CHECK(std::get<IndexPair>(code[9].immediate) == IndexPair{2, 2});
  CHECK(index_of(code[10]) == 2);
  CHECK(std::get<IndexPair>(code[11].immediate) == IndexPair{2, 1});
  CHECK(index_of(code[12]) == 2);
  CHECK(index_of(code[14]) == 1);
  CHECK(index_of(code[16]) == 1);
  CHECK(index_of(code[17]) == 2);
  CHECK(index_of(code[19]) == 1);
  CHECK(index_of(code[20]) == 1);
  CHECK(index_of(code[21]) == 2);
  CHECK(std::get<IndexPair>(code[23].immediate) == IndexPair{0, 2});
  CHECK(index_of(code[24]) == 2);
  CHECK(index_of(code[26]) == 2);
  CHECK(index_of(code[27]) == 0);
  CHECK(std::get<core::MemoryArg>(code[29].immediate) == core::MemoryArg{2, 127});
  const auto bytes = core::encode_module(result);
  const auto reparsed = core::decode_module(bytes);
  core::validate_module(reparsed);
  CHECK(core::encode_module(reparsed) == bytes);
}

void host_imports_and_reexports() {
  Module provider;
  provider.types = {{}};
  provider.imports = {{"host", "origin", Index{0}}, {"host", "ram", core::MemoryType{{1, 6}}},
                      {"host", "other", core::GlobalType{i32, false}}};
  provider.exports = {{"middle", func, 0}};

  Module facade;
  facade.types = {{{i64}, {}}, {}};
  facade.imports = {{"ns", "middle", Index{1}}, {"host", "other", core::GlobalType{i32, false}},
                    {"host", "ram", core::MemoryType{{2, 4}}}};
  facade.exports = {{"public", func, 0}};

  Module client;
  client.types = {{}};
  client.imports = {{"ns", "public", Index{0}}, {"host", "origin", Index{0}}};
  client.functions = {{0, {0, {}, {op(0x10, Index{0}), op(0x10, Index{1}), op(0x0b)}}}};

  const auto result = link({client, facade, provider});
  CHECK(result.imports.size() == 3);
  CHECK(result.imports[0].name == "origin");
  CHECK(result.imports[1].name == "other");
  CHECK(result.imports[2].name == "ram");
  CHECK(std::get<core::MemoryType>(result.imports[2].desc).limits == core::Limits{2, 4});
  CHECK(result.exports[0].index == 0 && result.exports[1].index == 0);
  CHECK(index_of(result.functions[0].body.instructions[0]) == 0);
  CHECK(index_of(result.functions[0].body.instructions[1]) == 0);

  Module self;
  self.types = {{}};
  self.imports = {{"host", "self", Index{0}}};
  self.exports = {{"self", func, 0}};
  CHECK(link({self}).imports.size() == 1);
}

void starts_and_global_chains() {
  Module left;
  left.types = {{}};
  left.imports = {{"right", "g2", core::GlobalType{i32, false}}, {"host", "init", Index{0}}};
  left.globals = {{{i32, false}, global_get(0)}};
  left.exports = {{"g1", global, 1}};
  left.start = 0;

  Module middle;
  middle.imports = {{"right", "g3", core::GlobalType{i32, false}}};
  middle.globals = {{{i32, false}, global_get(0)}};
  middle.exports = {{"g2", global, 1}};

  Module right;
  right.types = {{}};
  right.globals = {{{i32, false}, constant(17)}};
  right.exports = {{"g3", global, 0}};
  right.functions = {{0, {0, {}, {op(0x0b)}}}};
  right.start = 0;

  const auto result = link({left, middle, right});
  CHECK(result.globals.size() == 3);
  for (const auto& entry : result.globals)
    CHECK(entry.init == constant(17));
  CHECK(result.start == 2);
  CHECK(result.functions.size() == 2);
  CHECK(result.functions[1].body.instructions ==
        Program{op(0x10, Index{0}), op(0x10, Index{1}), op(0x0b)});
  CHECK(link({right}).start == 0);
  CHECK(!link({}).start);
}

void namespaces() {
  Module left;
  left.types = {{{}, {i32}}};
  left.functions = {{0, {0, {}, constant(1)}}};
  left.exports = {{"entry", func, 0}};
  Module right = left;
  right.functions[0].body.instructions = constant(2);
  Module use;
  use.types = left.types;
  use.imports = {{"right", "entry", Index{0}}, {"host", "entry", Index{0}}, {"left", "entry", Index{0}}};
  use.functions = {{0, {0, {}, {
      op(0x10, Index{0}), op(0x10, Index{1}), op(0x6a), op(0x10, Index{2}), op(0x6a), op(0x0b)}}}};
  use.exports = {{"run", func, 3}};

  const std::array named{core::LinkInput{"left", &left}, core::LinkInput{"right", &right},
                         core::LinkInput{"use", &use}};
  const auto result = core::link_modules(named);
  CHECK(result.imports.size() == 1 && result.imports[0].module == "host");
  CHECK(result.exports[0].name == "left.entry" && result.exports[0].index == 1);
  CHECK(result.exports[1].name == "right.entry" && result.exports[1].index == 2);
  CHECK(result.exports[2].name == "run" && result.exports[2].index == 3);
  const auto& code = result.functions[2].body.instructions;
  CHECK(index_of(code[0]) == 2);
  CHECK(index_of(code[1]) == 0);
  CHECK(index_of(code[3]) == 1);

  rejects("duplicate input namespace 'left'", [&] {
    core::link_modules(std::array{named[0], core::LinkInput{"left", &right}});
  });
  rejects("missing named input module", [&] {
    core::link_modules(std::array{core::LinkInput{"bad", nullptr}});
  });
  rejects("input namespace must not be empty", [&] {
    core::link_modules(std::array{core::LinkInput{"", &left}});
  });
  right.exports[0].name = "other";
  rejects("missing export for import 'right.entry'", [&] { core::link_modules(named); });
  right.exports[0].name = "entry";
  use.exports[0].name = "left.entry";
  rejects("output export collision 'left.entry'", [&] { core::link_modules(named); });
}

void diagnostics() {
  Module p;
  p.types = {{{}, {i32}}};
  p.functions = {{0, {0, {}, constant(1)}}};
  p.exports = {{"f", func, 0}};

  rejects("duplicate export 'f'", [&] { link({p, p}); });
  Module q;
  q.types = {{}};
  q.imports = {{"ns", "f", Index{0}}};
  rejects("incompatible function import 'ns.f'", [&] { link({p, q}); });
  q.imports = {{"ns", "f", core::TableType{funcref, {1, 2}}}};
  rejects("expects a table", [&] { link({p, q}); });

  Module r;
  r.memories = {{{1, 2}}};
  rejects("independent memories", [&] { link({r, r}); });
  r.exports = {{"mem", memory, 0}};
  q.imports = {{"ns", "mem", core::MemoryType{{1, 1}}}};
  rejects("incompatible memory import 'ns.mem'", [&] { link({r, q}); });
  q.imports = {{"host", "mem", core::MemoryType{{1, 1}}}};
  r = {};
  r.imports = {{"host", "mem", core::MemoryType{{2, 3}}}};
  rejects("conflicting host import requirements", [&] { link({q, r}); });

  Module a, b;
  a.types = b.types = {{}};
  a.imports = {{"a", "b", Index{0}}};
  a.exports = {{"a", func, 0}};
  b.imports = {{"b", "a", Index{0}}};
  b.exports = {{"b", func, 0}};
  rejects("cyclic import/export alias", [&] { link({a, b}); });
  // Unlike aliases with no definition, mutual recursion in actual code is legal.
  a.functions = b.functions = {{0, {0, {}, {op(0x10, Index{0}), op(0x0b)}}}};
  a.exports[0].index = b.exports[0].index = 1;
  const auto recursive = link({a, b});
  CHECK(recursive.imports.empty());
  CHECK(index_of(recursive.functions[0].body.instructions[0]) == 1);
  CHECK(index_of(recursive.functions[1].body.instructions[0]) == 0);
  a = {};
  b = {};
  a.imports = {{"a", "b", core::GlobalType{i32, false}}};
  a.globals = {{{i32, false}, global_get(0)}};
  a.exports = {{"a", global, 1}};
  b.imports = {{"b", "a", core::GlobalType{i32, false}}};
  b.globals = {{{i32, false}, global_get(0)}};
  b.exports = {{"b", global, 1}};
  rejects("cyclic linked global initializer", [&] { link({a, b}); });

  p.custom = {{"linking", {}}};
  rejects("unsupported Wasm object section 'linking'", [&] { link({p}); });
  p.custom = {{"reloc.CODE", {}}};
  rejects("unsupported Wasm object section 'reloc.CODE'", [&] { link({p}); });
  p.custom = {{"dylink.0", {}}};
  rejects("unsupported Wasm dynamic-linking section 'dylink.0'", [&] { link({p}); });
  p.custom.clear();
  p.functions[0].body.instructions = {{core::Opcode{0x10}, Index{127}, 51}, op(0x0b)};
  rejects("link input 0", [&] { link({p}); });
}

} // namespace

int main() {
  return test_main([] {
    calls_and_types();
    tables_globals_memory_and_segments();
    host_imports_and_reexports();
    starts_and_global_chains();
    namespaces();
    diagnostics();
  });
}
