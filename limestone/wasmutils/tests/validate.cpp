#include "wasmutils/WASM-Validate.hpp"
#include "../../tests/test.hpp"

#include <initializer_list>
#include <utility>

using limestone::wasmutils::Error;
namespace core = limestone::wasmutils::core;

namespace {

using Bytes = std::vector<std::uint8_t>;
using enum core::Type;

core::Program instructions(std::initializer_list<std::uint8_t> bytes) {
  return core::decode({bytes.begin(), bytes.size()});
}

core::Module function(core::Program body, core::FunctionType type = {}) {
  core::Module module;
  module.types.push_back(std::move(type));
  module.functions.push_back({0, {100, {}, std::move(body)}});
  return module;
}

void rejects(const core::Module& module, std::string_view diagnostic,
             std::optional<std::size_t> offset = std::nullopt) {
  try {
    core::validate_module(module);
  } catch (const Error& error) {
    if (std::string_view(error.what()).find(diagnostic) == std::string_view::npos ||
        (offset && error.offset != *offset))
      throw std::runtime_error("expected " + std::string(diagnostic) + " diagnostic" +
                               (offset ? " at " + std::to_string(*offset) : "") +
                               ", got: " + error.what());
    return;
  }
  throw std::runtime_error("expected validation failure: " + std::string(diagnostic));
}

void test_stack_and_locals() {
  auto module = function(instructions({0x20,0, 0x20,2, 0x0b}), {{i32}, {i32,i64}});
  module.functions[0].body.locals = {{0, v128}, {2, i64}, {1, f64}};
  core::validate_module(module);
  module.functions[0].body.instructions[1].immediate = std::uint32_t{4};
  rejects(module, "local index out of range", 2);

  // This valid run needs no allocation proportional to its encoded count.
  module = function({{{0x20}, std::uint32_t{UINT32_MAX - 1}, 99}, {{0x0b}, {}, 100}},
                    {{}, {externref}});
  module.functions[0].body.locals = {{UINT32_MAX, externref}};
  core::validate_module(module);
  module.functions[0].body.locals.push_back({1, i32});
  rejects(module, "too many local declarations", 100);

  core::validate_module(function(instructions({0, 0x6a, 0x0b}), {{}, {i32}}));
  rejects(function(instructions({0x6a, 0x0b}), {{}, {i32}}), "underflow", 0);
  rejects(function(instructions({0, 0x42,0, 0x6a, 0x0b}), {{}, {i32}}), "type mismatch", 3);
  rejects(function(instructions({0x41,0, 0, 0x41,0, 0x0b})), "unexpected operands", 5);
  rejects(function(instructions({0, 0x02,0x40, 0x1a, 0x0b, 0x0b})), "underflow", 3);

  core::validate_module(function(instructions({0, 0x1b, 0xd1, 0x0b}), {{}, {i32}}));
  core::validate_module(function(instructions({0xd0,0x6f, 0xd0,0x6f, 0x41,0, 0x1c,1,0x6f, 0x0b}),
                                 {{}, {externref}}));
  rejects(function(instructions({0xd0,0x6f, 0xd0,0x6f, 0x41,0, 0x1b, 0x0b}), {{}, {externref}}),
          "untyped select", 6);
  rejects(function(instructions({0, 0x1c,0, 0x0b})), "exactly one result type", 1);
  rejects(function(instructions({0, 0x1c,2,0x7f,0x7f, 0x0b})), "exactly one result type", 1);

  rejects(function({{{0x41}, {}, 84}, {{0x0b}, {}, 85}}), "unexpected immediate", 84);
  rejects(function({{{0xfe, 0x10}, {}, 90}, {{0x0b}, {}, 91}}), "unsupported Core 2.0 opcode", 90);
  rejects(function(instructions({0x0b, 0x01})), "after function end", 1);
  rejects(function(instructions({0x01})), "missing end", 100);
}

void test_decoded_module() {
  Bytes binary{
      0,0x61,0x73,0x6d,1,0,0,0,
      1,5,1,0x60,0,1,0x7f,
      3,2,1,0,
      10,6,1,4,0,0x41,1,0x0b
  };
  core::validate_module(core::decode_module(binary));
  binary[24] = 0x42; // Structurally valid i64.const instead of i32.const.
  rejects(core::decode_module(binary), "operand type mismatch", 26);
}

void test_control() {
  auto module = function(instructions({
      0x41,1, 0x42,2, 0x41,1,
      0x04,1, // if (param i32 i64) (result i64 i32)
        0x1a, 0x1a, 0x42,3, 0x41,4,
      0x05,
        0x1a, 0x1a, 0x42,5, 0x41,6,
      0x0b,
      0x0b
  }), {{}, {i64,i32}});
  module.types.push_back({{i32,i64}, {i64,i32}});
  core::validate_module(module);
  module.functions[0].body.instructions[3].immediate = core::BlockType{std::uint32_t{2}};
  rejects(module, "type index out of range", 6);
  module.functions[0].body.instructions[3].immediate = core::BlockType{std::uint32_t{1}};
  module.functions[0].body.instructions[10].opcode = {0x01};
  rejects(module, "unexpected operands", 21);

  module = function(instructions({0x41,0, 0x41,0, 0x04,1, 0x0b, 0x0b}), {{}, {i32}});
  module.types.push_back({{i32}, {i32}});
  core::validate_module(module); // Implicit else returns its incoming block parameters.
  rejects(function(instructions({0x41,0, 0x04,0x7f, 0x41,1, 0x0b, 0x0b}), {{}, {i32}}),
          "if without else", 6);
  rejects(function(instructions({0x05, 0x0b})), "else without matching if", 0);
  rejects(function(instructions({0x41,0, 0x04,0x40, 0x05, 0x05, 0x0b, 0x0b})),
          "else without matching if", 5);

  module = function(instructions({
      0x41,0, 0x03,1, 0x41,0, 0x0d,0, 0x1a, 0x42,0, 0x0b, 0x0b
  }), {{}, {i64}});
  module.types.push_back({{i32}, {i64}});
  core::validate_module(module); // Loop branches use input types; br_if preserves them.
  rejects(function(instructions({0x41,0, 0x02,0x40, 0x1a, 0x0b, 0x1a, 0x0b})), "underflow", 4);
  core::validate_module(function(instructions({0x42,1, 0x0c,0, 0x0b}), {{}, {i64}}));
  core::validate_module(function(instructions({0x42,1, 0x0f, 0x0b}), {{}, {i64}}));
  rejects(function(instructions({0, 0x0c,1, 0x0b})), "label depth out of range", 1);

  // Polymorphic branch operands can satisfy distinct same-arity label types.
  core::validate_module(function(instructions({
      0x02,0x7e, 0x02,0x7f, 0, 0x0e,1,0,1, 0x0b, 0x1a, 0, 0x0b, 0x1a, 0x0b
  })));
  rejects(function(instructions({
      0x02,0x7e, 0x02,0x7f, 0x41,0, 0x41,0, 0x0e,1,1,0, 0x0b, 0x1a, 0, 0x0b, 0x1a, 0x0b
  })), "branch table label", 8);
  rejects(function(instructions({
      0x02,0x40, 0x02,0x7f, 0, 0x0e,1,0,1, 0x0b, 0x1a, 0x0b, 0x0b
  })), "branch table label", 5);
}

void test_indices_and_module_rules() {
  auto module = function(instructions({
      0x20,0, 0x10,0,       // imported (param i32) (result i64)
      0x20,0, 0x41,0, 0x11,1,1, // call_indirect through the second table
      0x0b
  }), {{i32}, {i64,i64}});
  module.types.push_back({{i32}, {i64}});
  module.imports.push_back({"env", "callee", std::uint32_t{1}});
  module.imports.push_back({"env", "externrefs", core::TableType{externref, {0, std::nullopt}}});
  module.tables.push_back({funcref, {1, 2}});
  module.exports = {{"func", core::ExternalKind::function, 1},
                    {"table", core::ExternalKind::table, 1}};
  core::validate_module(module);
  module.functions[0].body.instructions[4].immediate = core::IndexPair{1,0};
  rejects(module, "requires a funcref table", 8);
  module.functions[0].body.instructions[4].immediate = core::IndexPair{1,1};
  module.functions[0].body.instructions[1].immediate = std::uint32_t{2};
  rejects(module, "function index out of range", 2);
  module.functions[0].body.instructions[1].immediate = std::uint32_t{0};
  module.exports[0].index = 2;
  rejects(module, "function index out of range");
  module.exports[0].index = 1;
  module.exports[1].name = "func";
  rejects(module, "duplicate export name");
  module.exports.pop_back();
  module.start = 0;
  rejects(module, "start function must have no parameters or results");
  module.start = 2;
  rejects(module, "function index out of range");
  module.start.reset();
  module.tables[0].limits.max = 0;
  rejects(module, "minimum exceeds maximum");
  module.tables[0].limits.max = 2;
  module.imports[0].desc = std::uint32_t{2};
  rejects(module, "type index out of range");

  module = function(instructions({0x24,0, 0x0b}));
  module.globals = {{{i32, false}, instructions({0x41,0, 0x0b})}};
  rejects(module, "immutable global", 0);
  module.globals[0].type.mutable_ = true;
  rejects(module, "underflow", 0);
  module.functions[0].body.instructions = instructions({0x41,1, 0x24,0, 0x0b});
  core::validate_module(module);

  module = function(instructions({0x3f,0, 0x0b}), {{}, {i32}});
  rejects(module, "memory index out of range");
  module.imports.push_back({"env", "memory", core::MemoryType{{65536, 65536}}});
  core::validate_module(module);
  module.memories.push_back({{0, std::nullopt}});
  rejects(module, "at most one memory");
  module.imports.clear();
  module.memories[0].limits.max = 65537;
  rejects(module, "limits exceed");
}

void test_initializers_and_segments() {
  auto module = function(instructions({0xd2,0, 0x1a, 0x0b}));
  rejects(module, "undeclared function reference", 0);
  module.start = 0;
  rejects(module, "undeclared function reference", 0);
  module.elements.push_back({core::SegmentMode::declarative, 0, {}, funcref,
                             {instructions({0xd2,0, 0x0b})}});
  core::validate_module(module);
  module.elements[0].init[0][0].immediate = std::uint32_t{1};
  rejects(module, "function index out of range");
  module.elements.clear();
  module.globals.push_back({{funcref, false}, instructions({0xd2,0, 0x0b})});
  core::validate_module(module);
  module.globals.clear();
  module.exports.push_back({"func", core::ExternalKind::function, 0});
  core::validate_module(module);

  module.imports.push_back({"env", "immutable", core::GlobalType{i32, false}});
  module.globals.push_back({{i32, false}, instructions({0x23,0, 0x0b})});
  module.globals.push_back({{v128, true}, {
      {{0xfd, 0x0c}, core::VectorBytes{}, 42}, {{0x0b}, {}, 60}
  }});
  module.tables.push_back({externref, {0, std::nullopt}});
  module.elements.push_back({core::SegmentMode::active, 0, instructions({0x23,0, 0x0b}), externref,
                             {instructions({0xd0,0x6f, 0x0b})}});
  module.memories.push_back({{0, std::nullopt}});
  module.data.push_back({core::SegmentMode::active, 0, instructions({0x41,5, 0x0b}), {1, 2, 3}});
  core::validate_module(module); // Initial segment bounds are checked at instantiation.
  module.data[0].offset = instructions({0x42,0, 0x0b});
  rejects(module, "constant expression result type mismatch");
  module.data[0].offset = instructions({0x41,0, 0x0b});
  module.data[0].mode = core::SegmentMode::passive;
  rejects(module, "passive data segment carries an active memory or offset");
  module.data[0].mode = core::SegmentMode::active;
  module.elements[0].mode = core::SegmentMode::declarative;
  rejects(module, "inactive element segment carries an active table or offset");
  module.elements[0].mode = core::SegmentMode::active;
  module.elements[0].type = funcref;
  rejects(module, "segment and table types do not match");
  module.elements[0].type = externref;
  module.globals[0].init[0].immediate = std::uint32_t{1};
  rejects(module, "immutable imported globals");
  module.globals[0].init[0].immediate = std::uint32_t{0};
  module.imports[0].desc = core::GlobalType{i32, true};
  rejects(module, "immutable imported globals");
  module.globals[0].init = instructions({0, 0x0b});
  rejects(module, "not allowed in a constant expression");
  module.globals[0].init = instructions({0x41,0, 0x41,1, 0x0b});
  rejects(module, "expected one constant instruction");
}

void test_bulk_memory_and_simd() {
  auto module = function(instructions({
      0x41,0, 0x41,0, 0x41,0, 0xfc,8,0,0, // memory.init data=0 memory=0
      0xfc,9,0,
      0x41,0, 0x41,0, 0x41,0, 0xfc,12,1,0, // table.init elem=1 table=0
      0xfc,13,1,
      0x0b
  }));
  module.memories.push_back({{1, 1}});
  module.data.push_back({core::SegmentMode::passive, 0, {}, {1}});
  module.tables.push_back({externref, {1, 1}});
  module.elements = {{core::SegmentMode::passive, 0, {}, funcref, {}},
                     {core::SegmentMode::passive, 0, {}, externref, {}}};
  rejects(module, "requires a data count section", 6);
  module.data_count = 2;
  rejects(module, "data count does not match");
  module.data_count = 1;
  core::validate_module(module);
  module.functions[0].body.instructions[3].immediate = std::uint32_t{1};
  rejects(module, "data segment index out of range", 6);
  module.functions[0].body.instructions[3].immediate = std::uint32_t{0};
  module.functions[0].body.instructions[8].immediate = core::IndexPair{0,0};
  rejects(module, "segment and table types do not match", 19);

  module = function(instructions({
      0x41,0, 0x41,0, 0xfd,0,4,0, 0xfd,0x57,3,0,1, 0xfd,0x1d,1, 0x0b
  }), {{}, {i64}});
  module.memories.push_back({{1, 1}});
  core::validate_module(module);
  module.functions[0].body.instructions[3].immediate = core::MemoryLane{{3,0}, 2};
  rejects(module, "lane index out of range", 8);
  module.functions[0].body.instructions[3].immediate = core::MemoryLane{{4,0}, 1};
  rejects(module, "alignment exceeds", 8);
  module.functions[0].body.instructions = instructions({0, 0x2c,1,0, 0x0b});
  rejects(module, "alignment exceeds", 1);

  core::VectorBytes shuffle{};
  shuffle[15] = 31;
  module = function({{{0}, {}, 0}, {{0xfd, 0x0d}, shuffle, 1}, {{0x0b}, {}, 19}}, {{}, {v128}});
  core::validate_module(module);
  shuffle[15] = 32;
  module.functions[0].body.instructions[1].immediate = shuffle;
  rejects(module, "shuffle lane index out of range", 1);

  // Check that every SIMD opcode in the binary inventory has a validator rule.
  for (const auto& info : core::opcodes()) {
    if (info.opcode.byte != 0xfd)
      continue;
    core::Immediate immediate;
    switch (info.format) {
    case core::Format::none: break;
    case core::Format::memory: immediate = core::MemoryArg{}; break;
    case core::Format::memory_lane: immediate = core::MemoryLane{}; break;
    case core::Format::lane: immediate = std::uint8_t{}; break;
    case core::Format::vector_bytes: immediate = core::VectorBytes{}; break;
    default: CHECK(false && "unknown SIMD immediate format");
    }
    module = function({{{0}, {}, 0}, {info.opcode, immediate, 1}, {{0}, {}, 20}, {{0x0b}, {}, 21}});
    module.memories.push_back({{1, 1}});
    core::validate_module(module);
  }
}

} // namespace

int main() {
  return test_main([] {
    test_stack_and_locals();
    test_decoded_module();
    test_control();
    test_indices_and_module_rules();
    test_initializers_and_segments();
    test_bulk_memory_and_simd();
  });
}
