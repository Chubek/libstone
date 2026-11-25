#include "wasmutils/WASM-Instance.hpp"
#include "wasmutils/WASM-Object-Link.hpp"
#include "wasmutils/WASM-Validate.hpp"
#include "../../tests/test.hpp"

#include <array>
#include <fstream>
#include <iterator>

namespace wasm = limestone::wasmutils;
namespace core = wasm::core;
namespace {

using Bytes = std::vector<std::uint8_t>;
using Index = std::uint32_t;
using core::Type;
using namespace std::string_view_literals;

constexpr auto i32 = Type::i32;
constexpr auto i64 = Type::i64;

void name(Bytes& out, std::string_view value) {
  CHECK(value.size() < 128);
  out.push_back(static_cast<std::uint8_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void section(Bytes& out, std::uint8_t id, const Bytes& payload) {
  CHECK(payload.size() < 128);
  out.push_back(id);
  out.push_back(static_cast<std::uint8_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
}

void custom(Bytes& out, std::string_view tag, const Bytes& data) {
  Bytes payload;
  name(payload, tag);
  payload.insert(payload.end(), data.begin(), data.end());
  section(out, 0, payload);
}

Bytes definition(Index answer = 42, std::uint8_t flags = 4, Type result = i32) {
  core::Module module;
  module.types = {{{}, {result}}};
  const auto value = result == i32
      ? core::Instruction{{0x41}, answer}
      : core::Instruction{{0x42}, std::uint64_t{answer}};
  module.functions = {{0, {0, {}, {value, {{0x0b}, {}}}}}};
  Bytes symbols{1, 0, flags, 0};
  name(symbols, "answer");
  Bytes linking{2};
  section(linking, 8, symbols);
  module.custom = {{"linking", linking}};
  return core::encode_module(module);
}

Bytes consumer(Bytes relocation = {3, 1, 0, 4, 0}, std::uint8_t import_flags = 16) {
  Bytes binary{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
  section(binary, 1, {1, 0x60, 0, 1, 0x7f});
  Bytes import{1};
  name(import, "host");
  name(import, "answer");
  import.insert(import.end(), {0, 0});
  section(binary, 2, import);
  section(binary, 3, {1, 0});
  section(binary, 10, {1, 8, 0, 0x10, 0x80, 0x80, 0x80, 0x80, 0, 0x0b});
  Bytes symbols{2, 0, import_flags, 0, 0, 0, 1};
  name(symbols, "run");
  Bytes linking{2};
  section(linking, 8, symbols);
  custom(binary, "linking", linking);
  // The target section ordinal, not the conventional name suffix, is authoritative.
  custom(binary, "reloc.renamed", relocation);
  return binary;
}

template<class Run>
void rejects(std::string_view message, Run run, bool with_offset = false) {
  try {
    run();
  } catch (const wasm::Error& error) {
    CHECK(std::string_view(error.what()).find(message) != std::string_view::npos);
    if (with_offset)
      CHECK(error.offset > 8);
    return;
  }
  CHECK(false && "expected an object link diagnostic");
}

core::Module link(Bytes const& caller, Bytes const& provider,
                  std::span<const std::string_view> exports = {}) {
  return core::link_objects(std::array{core::ObjectLinkInput{"caller.o", caller},
      core::ObjectLinkInput{"provider.o", provider}}, {exports});
}

void synthetic() {
  const auto caller = consumer();
  const auto provider = definition();
  const auto output = link(caller, provider, std::array{"answer"sv});
  CHECK(output.functions.size() == 2);
  CHECK(output.types.size() == 1);
  CHECK(output.imports.empty() && output.custom.empty() && !output.start);
  const auto encoded = core::encode_module(output);
  CHECK(encoded == core::encode_module(link(caller, provider, std::array{"answer"sv})));
  core::Instance instance(core::decode_module(encoded));
  CHECK(instance.invoke("run") == std::vector<core::Value>{{i32, 42}});
  CHECK(instance.invoke("answer") == std::vector<core::Value>{{i32, 42}});

  // An unmatched strong imported function keeps the original module/field.
  auto host_output = core::link_objects(std::array{core::ObjectLinkInput{"caller.o", caller}});
  const std::array hosts{core::HostFunction{"host", "answer", [](auto) {
    return std::vector<core::Value>{{i32, 19}};
  }}};
  core::Instance with_host(std::move(host_output), hosts);
  CHECK(with_host.invoke("run") == std::vector<core::Value>{{i32, 19}});

  const auto weak = definition(17, 5);
  const auto duplicate = definition(13);
  const auto inputs = std::array{core::ObjectLinkInput{"weak.o", weak},
      core::ObjectLinkInput{"caller.o", caller}, core::ObjectLinkInput{"provider.o", provider}};
  const auto replaced = core::link_objects(inputs);
  CHECK(replaced.functions.size() == 2);
  core::Instance with_weak(replaced);
  CHECK(with_weak.invoke("run") == std::vector<core::Value>{{i32, 42}});

  rejects("duplicate definition of symbol 'answer'", [&] { link(provider, duplicate); }, true);
  rejects("incompatible type for symbol 'answer'", [&] { link(caller, definition(13, 4, i64)); },
          true);
  rejects("requested export 'absent' is not a defined object symbol",
          [&] { link(caller, provider, std::array{"absent"sv}); });
  rejects("caller.o': unsupported Wasm relocation R_WASM_MEMORY_ADDR_LEB64", [&] {
    link(consumer({3, 1, 14, 4, 0}), provider);
  }, true);
  rejects("references a symbol of the wrong kind", [&] {
    link(consumer({3, 1, 7, 4, 0}), provider);
  }, true);
  rejects("symbol table index 2 is out of range", [&] {
    link(consumer({3, 1, 0, 4, 2}), provider);
  }, true);
  rejects("relocation offset is outside the target section", [&] {
    link(consumer({3, 1, 0, 9, 0}), provider);
  }, true);
  rejects("does not target a supported CODE operand", [&] {
    link(consumer({3, 1, 0, 3, 0}), provider);
  }, true);
  rejects("requires a 5-byte padded CODE operand", [&] {
    // Leave enough bytes in the section but shorten the actual immediate.
    auto bad_caller = consumer();
    auto baseline = core::decode_module(bad_caller);
    const auto imm = baseline.functions[0].body.instructions[0].offset + 1;
    bad_caller[imm] = 0;
    for (unsigned i = 1; i < 5; ++i)
      bad_caller[imm + i] = 0x01; // nop
    link(bad_caller, provider);
  }, true);
  rejects("duplicate relocation at the same target offset", [&] {
    link(consumer({3, 2, 0, 4, 0, 0, 4, 0}), provider);
  }, true);
  rejects("targets an unsupported section", [&] {
    link(consumer({2, 1, 0, 4, 0}), provider);
  }, true);
  const auto weak_caller = consumer({3, 1, 0, 4, 0}, 17);
  rejects("unresolved weak symbol 'answer'", [&] {
    core::link_objects(std::array{core::ObjectLinkInput{"weak-caller.o", weak_caller}});
  }, true);

  auto a = caller;
  auto b = provider;
  Bytes features{1, '+'};
  name(features, "simd128");
  custom(a, "target_features", features);
  features[1] = '-';
  custom(b, "target_features", features);
  rejects("conflicting target feature 'simd128'", [&] { link(a, b); }, true);

  Bytes empty{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
  rejects("missing linking section", [&] { link(empty, provider); });
  custom(empty, "linking", {1});
  rejects("unsupported Wasm linking metadata version 1", [&] { link(empty, provider); }, true);
}

Bytes read_file(char const* path) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void real_objects(char const* caller_path, char const* provider_path) {
  const auto caller = read_file(caller_path);
  const auto provider = read_file(provider_path);
  constexpr std::array exports{"run"sv, "value"sv, "pointer"sv, "get_callback"sv, "get_value"sv,
                               "__wasm_call_ctors"sv, "__indirect_function_table"sv};
  for (const auto reversed : {false, true}) {
    auto output = reversed ? link(provider, caller, exports) : link(caller, provider, exports);
    CHECK(output.imports.empty() && output.custom.empty() && !output.start);
    CHECK(output.memories.size() == 1 && output.tables.size() == 1);
    CHECK(!output.data_count);
    const auto encoded = core::encode_module(output);
    const auto decoded = core::decode_module(encoded);
    core::validate_module(decoded);
    CHECK(core::encode_module(decoded) == encoded);
    core::Instance instance(decoded);
    CHECK(instance.invoke("public_run") == std::vector<core::Value>{{i32, 112}});
    CHECK(instance.invoke("__wasm_call_ctors").empty());
    CHECK(instance.invoke("run") == std::vector<core::Value>{{i32, 211}});
    const auto value_address = instance.get_global("value").bits;
    CHECK(instance.invoke("get_value") == std::vector<core::Value>{{i32, value_address}});
    CHECK(instance.read_memory("memory", value_address, 4) == Bytes{64, 0, 0, 0});
    const auto pointer_address = instance.get_global("pointer").bits;
    const auto pointer = instance.read_memory("memory", pointer_address, 4);
    CHECK((pointer[0] & 31) == 4); // Aligned target segment plus a four-byte addend.
    CHECK(instance.get_table("__indirect_function_table", 0).bits == 0);
    const auto slot = instance.invoke("get_callback").front().bits;
    CHECK(slot == (reversed ? 2 : 1));
    CHECK(instance.get_table("__indirect_function_table", slot).bits != 0);
  }
}

} // namespace

int main(int argc, char** argv) {
  return test_main([&] {
    CHECK(argc == 1 || argc == 3);
    if (argc == 3)
      real_objects(argv[1], argv[2]);
    else
      synthetic();
  });
}
