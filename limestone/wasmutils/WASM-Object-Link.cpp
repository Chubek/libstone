#include "WASM-Object-Link.hpp"
#include "WASM-Validate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace limestone::wasmutils::core {
namespace {

using Index = std::uint32_t;
using Bytes = std::span<const std::uint8_t>;
using Kind = ExternalKind;

[[noreturn]] void bad(std::size_t offset, const std::string& text) {
  throw Error(offset, text);
}

Index index_of(std::uint64_t value, std::size_t offset = 0) {
  if (value > std::numeric_limits<Index>::max())
    bad(offset, "value exceeds the wasm32 range");
  return static_cast<Index>(value);
}

class Reader {
public:
  Reader(Bytes bytes, std::size_t offset = 0) : bytes_(bytes), offset_(offset) {}
  std::size_t offset() const { return offset_; }
  Bytes rest() const { return bytes_; }
  Bytes take(std::size_t size, std::string_view field) {
    if (size > bytes_.size())
      bad(offset_, "truncated " + std::string(field));
    const auto result = bytes_.first(size);
    bytes_ = bytes_.subspan(size);
    offset_ += size;
    return result;
  }
  std::uint8_t byte(std::string_view field) { return take(1, field).front(); }
  Index leb(std::string_view field, bool sign = false) {
    const auto start = offset_;
    Index value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
      const auto next = byte(field);
      if (shift == 28 && (sign ? next > 7 && (next < 0x78 || next > 0x7f)
                               : next > 0x0f))
        bad(start, "invalid LEB128 for " + std::string(field));
      value |= Index(next & 0x7f) << shift;
      if (next < 0x80) {
        if (sign && shift < 28 && (next & 0x40))
          value |= ~Index{0} << (shift + 7);
        return value;
      }
    }
    bad(start, "invalid LEB128 for " + std::string(field));
  }
  Index count(std::string_view field, std::size_t minimum) {
    const auto n = leb(field);
    if (n > rest().size() / minimum)
      bad(offset_, "truncated " + std::string(field));
    return n;
  }
  Reader payload(std::string_view field) {
    const auto size = leb(field);
    const auto start = offset();
    return {take(size, field), start};
  }
  std::string name(std::string_view field) {
    const auto text = payload(field);
    const auto bytes = text.rest();
    // Object names can become Wasm exports, which require well-formed UTF-8.
    for (std::size_t i = 0; i < bytes.size();) {
      const auto lead = bytes[i++];
      if (lead < 0x80)
        continue;
      const unsigned n = lead < 0xc2 ? 0 : lead < 0xe0 ? 1 :
                         lead < 0xf0 ? 2 : lead <= 0xf4 ? 3 : 0;
      if (!n || n > bytes.size() - i ||
          (lead == 0xe0 && bytes[i] < 0xa0) || (lead == 0xed && bytes[i] >= 0xa0) ||
          (lead == 0xf0 && bytes[i] < 0x90) || (lead == 0xf4 && bytes[i] >= 0x90))
        bad(text.offset(), "invalid UTF-8 in " + std::string(field));
      for (unsigned j = 0; j < n; ++j)
        if ((bytes[i++] & 0xc0) != 0x80)
          bad(text.offset(), "invalid UTF-8 in " + std::string(field));
    }
    return {bytes.begin(), bytes.end()};
  }
  void end(std::string_view field) const {
    if (!bytes_.empty())
      bad(offset_, "trailing bytes in " + std::string(field));
  }
private:
  Bytes bytes_;
  std::size_t offset_;
};

enum class SymKind : std::uint8_t { function, data, global, section, tag, table };
constexpr Index weak = 1, local = 2, binding = 3, hidden = 4, undefined = 0x10,
                exported = 0x20, explicit_name = 0x40, no_strip = 0x80;

struct Symbol {
  SymKind kind{};
  Index flags{}, index{}, offset{}, size{};
  std::string name;
  std::size_t at{};
  bool is_undefined() const { return (flags & undefined) != 0; }
  bool is_weak() const { return (flags & binding) == weak; }
  bool is_local() const { return (flags & binding) == local; }
};

Kind kind_of(SymKind kind) {
  switch (kind) {
  case SymKind::function: return Kind::function;
  case SymKind::global: return Kind::global;
  case SymKind::table: return Kind::table;
  default: bad(0, "symbol has no Wasm index space");
  }
}

constexpr auto space(Kind kind) { return static_cast<std::size_t>(kind); }
constexpr std::array entity_kinds{Kind::function, Kind::global, Kind::table, Kind::memory};

enum Reloc : std::uint8_t {
  function_leb = 0, slot_sleb = 1, slot_i32 = 2, address_leb = 3, address_sleb = 4,
  address_i32 = 5, type_leb = 6, global_leb = 7, global_i32 = 13, table_leb = 20,
  function_i32 = 26,
};

std::string relocation_name(unsigned type) {
  constexpr std::array names{
      "FUNCTION_INDEX_LEB", "TABLE_INDEX_SLEB", "TABLE_INDEX_I32", "MEMORY_ADDR_LEB",
      "MEMORY_ADDR_SLEB", "MEMORY_ADDR_I32", "TYPE_INDEX_LEB", "GLOBAL_INDEX_LEB",
      "FUNCTION_OFFSET_I32", "SECTION_OFFSET_I32", "TAG_INDEX_LEB", "MEMORY_ADDR_REL_SLEB",
      "TABLE_INDEX_REL_SLEB", "GLOBAL_INDEX_I32", "MEMORY_ADDR_LEB64", "MEMORY_ADDR_SLEB64",
      "MEMORY_ADDR_I64", "MEMORY_ADDR_REL_SLEB64", "TABLE_INDEX_SLEB64", "TABLE_INDEX_I64",
      "TABLE_NUMBER_LEB", "MEMORY_ADDR_TLS_SLEB", "FUNCTION_OFFSET_I64", "MEMORY_ADDR_LOCREL_I32",
      "TABLE_INDEX_REL_SLEB64", "MEMORY_ADDR_TLS_SLEB64", "FUNCTION_INDEX_I32"};
  return type < names.size() ? std::string("R_WASM_") + names[type] : std::to_string(type);
}

struct Fixup {
  Reloc type;
  Index index;
  std::size_t at;
  std::int32_t addend{};
};

bool is_word(Reloc type) {
  return type == slot_i32 || type == address_i32 || type == global_i32 ||
         type == function_i32;
}

bool is_address(Reloc type) {
  return type == address_leb || type == address_sleb || type == address_i32;
}

SymKind relocation_kind(Reloc type) {
  if (is_address(type)) return SymKind::data;
  if (type == global_leb || type == global_i32) return SymKind::global;
  if (type == table_leb) return SymKind::table;
  return SymKind::function;
}

struct Section {
  std::uint8_t id;
  std::size_t at;
  Bytes bytes;
  std::string name;
  std::size_t content;
};

struct DataInfo {
  std::size_t at{}; // Original byte payload offset.
  Index align{}, address{};
};

struct Entity {
  const Import* import{};
  std::optional<Index> symbol;
  std::optional<Index> output;
};

struct Object {
  ObjectLinkInput source;
  Module module;
  std::array<std::vector<Entity>, entity_kinds.size()> entities;
  std::array<std::size_t, entity_kinds.size()> imported{};
  std::vector<Index> types;
  std::vector<Symbol> symbols;
  std::vector<DataInfo> data;
  std::vector<std::pair<Index, Index>> ctors;
  std::map<std::string, std::pair<bool, std::size_t>, std::less<>> features;
  std::map<std::size_t, Fixup> code_fixups, data_fixups;
};

bool valid_section_symbol(const std::vector<Section>& sections, Index i) {
  return i < sections.size() &&
         (sections[i].id == 0 || sections[i].id == 10 || sections[i].id == 11);
}

const Symbol& symbol_at(const Object& object, Index index, std::size_t at) {
  if (index >= object.symbols.size())
    bad(at, "symbol table index " + std::to_string(index) + " is out of range");
  return object.symbols[index];
}

void read_symbols(Object& object, Reader in, const std::vector<Section>& sections) {
  for (auto n = in.count("symbol table", 2); n; --n) {
    const auto at = in.offset();
    const auto kind = static_cast<SymKind>(in.byte("symbol kind"));
    const auto flags = in.leb("symbol flags");
    Symbol symbol{kind, flags, 0, 0, 0, {}, at};
    if (flags & ~(binding | hidden | undefined | exported | explicit_name | no_strip))
      bad(at, "unsupported symbol flags (TLS, absolute symbols, or unknown flags)");
    if ((flags & binding) == binding)
      bad(at, "unsupported common symbol binding");
    if (symbol.is_local() && (symbol.is_undefined() || (flags & exported)))
      bad(at, "local symbol cannot be undefined or exported");
    switch (kind) {
    case SymKind::function: case SymKind::global: case SymKind::table: {
      auto& slots = object.entities[space(kind_of(kind))];
      symbol.index = in.leb("symbol entity index");
      if (symbol.index >= slots.size())
        bad(at, "symbol entity index is out of range");
      auto& entity = slots[symbol.index];
      if (symbol.is_undefined() != (entity.import != nullptr))
        bad(at, "symbol undefined flag does not match its entity's import status");
      symbol.name = !entity.import || (flags & explicit_name)
                        ? in.name("symbol name") : entity.import->name;
      if (entity.symbol)
        bad(at, "unsupported multiple symbols for one Wasm entity: '" + symbol.name + "'");
      entity.symbol = index_of(object.symbols.size(), at);
      break;
    }
    case SymKind::data:
      symbol.name = in.name("data symbol name");
      if (!symbol.is_undefined()) {
        symbol.index = in.leb("data symbol segment");
        symbol.offset = in.leb("data symbol offset");
        symbol.size = in.leb("data symbol size");
        if (symbol.index >= object.module.data.size() ||
            std::uint64_t(symbol.offset) + symbol.size >
                object.module.data[symbol.index].bytes.size())
          bad(at, "data symbol '" + symbol.name + "' exceeds its segment bounds");
      }
      break;
    case SymKind::section:
      symbol.index = in.leb("section symbol index");
      if (!symbol.is_local() || !valid_section_symbol(sections, symbol.index))
        bad(at, "invalid section symbol");
      break;
    default: bad(at, "unsupported Wasm object symbol kind " +
                          std::to_string(static_cast<unsigned>(kind)));
    }
    object.symbols.push_back(std::move(symbol));
  }
  in.end("symbol table");
}

void read_linking(Object& object, Reader in, const std::vector<Section>& sections) {
  const auto at = in.offset();
  const auto version = in.leb("linking version");
  if (version != 2)
    bad(at, "unsupported Wasm linking metadata version " + std::to_string(version));
  std::set<std::uint8_t> seen;
  while (!in.rest().empty()) {
    const auto sub_at = in.offset();
    const auto id = in.byte("linking subsection ID");
    auto sub = in.payload("linking subsection");
    if (!seen.insert(id).second)
      bad(sub_at, "duplicate linking subsection " + std::to_string(id));
    switch (id) {
    case 5: {
      auto n = sub.count("segment info", 3);
      if (n != object.data.size())
        bad(sub_at, "linking segment info count does not match the data section");
      for (auto& data : object.data) {
        const auto segment_at = sub.offset();
        const auto name = sub.name("segment name");
        data.align = sub.leb("segment alignment");
        const auto flags = sub.leb("segment flags");
        if (data.align >= 32)
          bad(segment_at, "segment '" + name + "' alignment exceeds wasm32 range");
        if (flags & ~Index{5})
          bad(segment_at, "unsupported flags (TLS or unknown flags) for segment '" + name + "'");
      }
      break;
    }
    case 6:
      if (!seen.contains(8))
        bad(sub_at, "init functions must follow the linking symbol table");
      for (auto n = sub.count("init functions", 2); n; --n) {
        const auto entry_at = sub.offset();
        const auto priority = sub.leb("init function priority");
        const auto index = sub.leb("init function symbol index");
        const auto& symbol = symbol_at(object, index, entry_at);
        if (symbol.kind != SymKind::function || symbol.is_undefined())
          bad(entry_at, "init function must refer to a defined function symbol");
        object.ctors.emplace_back(priority, index);
      }
      break;
    case 7:
      if (sub.leb("COMDAT count") != 0)
        bad(sub_at, "Wasm object COMDAT groups are not supported");
      break;
    case 8: read_symbols(object, sub, sections); continue;
    case 9: {
      const auto arch = sub.name("object target architecture");
      if (arch != "wasm32")
        bad(sub_at, "unsupported Wasm object target architecture '" + arch + "'");
      break;
    }
    default: bad(sub_at, "unsupported linking subsection " + std::to_string(id));
    }
    sub.end("linking subsection");
  }
  if (!object.data.empty() && !seen.contains(5))
    bad(at, "Wasm object data requires a linking segment info subsection");
}

void read_relocations(Object& object, const Section& section, std::size_t section_index,
                      const std::vector<Section>& sections) {
  Reader in(section.bytes.subspan(section.content - section.at), section.content);
  const auto target = in.leb("relocation target section index");
  if (target >= section_index || target >= sections.size())
    bad(section.content, section.name + " does not reference an earlier section");
  const auto& destination = sections[target];
  if (destination.id != 10 && destination.id != 11)
    bad(section.content, section.name + " targets an unsupported section (only CODE and DATA)");
  auto& fixups = destination.id == 10 ? object.code_fixups : object.data_fixups;
  for (auto n = in.count("relocation entries", 3); n; --n) {
    const auto at = in.offset();
    const auto type = static_cast<Reloc>(in.byte("relocation type"));
    switch (type) {
    case function_leb: case slot_sleb: case slot_i32: case address_leb: case address_sleb:
    case address_i32: case type_leb: case global_leb: case global_i32: case table_leb:
    case function_i32:
      break;
    default: bad(at, "unsupported Wasm relocation " + relocation_name(type));
    }
    const auto offset = in.leb("relocation offset");
    const auto index = in.leb("relocation index");
    Fixup fixup{type, index, at};
    if (is_address(type))
      fixup.addend = std::bit_cast<std::int32_t>(in.leb("relocation addend", true));
    if (type == type_leb) {
      if (index >= object.module.types.size())
        bad(at, "relocation type index is out of range");
    } else if (symbol_at(object, index, at).kind != relocation_kind(type)) {
      bad(at, relocation_name(type) + " references a symbol of the wrong kind");
    }
    if (std::uint64_t(offset) + (is_word(type) ? 4 : 5) > destination.bytes.size())
      bad(at, "relocation offset is outside the target section");
    if (!fixups.emplace(destination.at + offset, fixup).second)
      bad(at, "duplicate relocation at the same target offset");
  }
  in.end(section.name);
}

void read_data_ranges(Object& object, const Section& section) {
  Reader in(section.bytes, section.at);
  in.leb("data segment count"); // The Core decoder already checked the data grammar.
  for (std::size_t i = 0; i < object.data.size(); ++i) {
    const auto at = in.offset();
    const auto flags = in.leb("data segment flags");
    if (flags == 1)
      bad(at, "passive object data segments are not supported");
    if (flags == 2 && in.leb("data memory index") != 0)
      bad(at, "object data must target memory zero");
    const auto& init = object.module.data[i].offset;
    if (init.size() != 2 || init.front().opcode != Opcode{0x41})
      bad(at, "object data offset must be i32.const");
    in.take(init.back().offset + 1 - in.offset(), "data offset expression");
    const auto size = in.leb("data segment size");
    object.data[i].at = in.offset();
    in.take(size, "data segment");
  }
  in.end("data section");
}

bool is_dynamic_import(const Import& import) {
  return import.module == "GOT.mem" || import.module == "GOT.func" ||
         (import.module == "env" && (import.name == "__memory_base" ||
          import.name == "__table_base" || import.name == "__tls_base"));
}

void read_object(Object& object) {
  object.module = decode_module(object.source.bytes);
  auto& module = object.module;
  for (const auto& import : module.imports)
    object.entities[import.desc.index()].push_back({&import, {}, {}});
  for (const auto kind : entity_kinds)
    object.imported[space(kind)] = object.entities[space(kind)].size();
  object.entities[space(Kind::function)].resize(object.imported[0] + module.functions.size());
  object.entities[space(Kind::global)].resize(object.imported[3] + module.globals.size());
  if (module.start)
    bad(0, "Wasm objects may not define a start section; use linking init functions");
  if (!module.memories.empty() || object.imported[space(Kind::memory)] > 1)
    bad(0, "object memory must be a single env.__linear_memory import");
  if (!module.tables.empty() || object.imported[space(Kind::table)] > 1)
    bad(0, "object table must be a single env.__indirect_function_table import");
  if (module.data_count && *module.data_count != module.data.size())
    bad(0, "object data count does not match the data section");
  object.data.resize(module.data.size());

  Reader binary(object.source.bytes.subspan(8), 8);
  std::vector<Section> sections;
  std::optional<std::size_t> linking;
  bool seen_features = false;
  while (!binary.rest().empty()) {
    const auto id = binary.byte("section ID");
    auto payload = binary.payload("section payload");
    Section section{id, payload.offset(), payload.rest(), {}, payload.offset()};
    if (id == 0) {
      section.name = payload.name("custom section name");
      section.content = payload.offset();
      if (section.name == "linking") {
        if (linking)
          bad(section.at, "duplicate Wasm linking section");
        linking = sections.size();
      } else if (section.name == "dylink" || section.name.starts_with("dylink.")) {
        bad(section.at, "unsupported Wasm dynamic-linking section '" + section.name + "'");
      } else if (section.name == "target_features") {
        if (std::exchange(seen_features, true))
          bad(section.at, "duplicate target_features section");
        for (auto n = payload.count("target features", 2); n; --n) {
          const auto at = payload.offset();
          const auto prefix = payload.byte("target feature prefix");
          const auto name = payload.name("target feature name");
          if (prefix != '+' && prefix != '-')
            bad(at, "unsupported target feature prefix for '" + name + "'");
          if (!object.features.try_emplace(name, prefix == '+', at).second)
            bad(at, "duplicate target feature '" + name + "'");
        }
        payload.end("target_features");
      }
    } else if (id == 11) {
      if (linking)
        bad(section.at, "the linking section must follow the data section");
      read_data_ranges(object, section);
    }
    sections.push_back(std::move(section));
  }
  if (!linking) {
    for (const auto& section : sections)
      if (section.name.starts_with("reloc."))
        bad(section.at, section.name + " requires a linking section");
    bad(0, "missing linking section (expected a relocatable Wasm object)");
  }
  const auto& metadata = sections[*linking];
  read_linking(object, Reader(metadata.bytes.subspan(metadata.content - metadata.at),
                              metadata.content), sections);
  // LLVM PIC objects use global-index relocations with function/data symbols;
  // identify the unsupported GOT ABI before interpreting those entries.
  for (const auto& entities : object.entities) {
    for (const auto& entity : entities) {
      const auto* import = entity.import;
      if (import && is_dynamic_import(*import)) {
        const auto at = entity.symbol ? object.symbols[*entity.symbol].at : 0;
        bad(at, "unsupported position-independent or TLS import '" + import->module + "." +
                    import->name + "'");
      }
    }
  }
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (!sections[i].name.starts_with("reloc."))
      continue;
    if (i < *linking)
      bad(sections[i].at, sections[i].name + " must follow the linking section");
    read_relocations(object, sections[i], i, sections);
  }
  module.custom.clear();
}

Program constant(Index value) { return {{{0x41}, value}, {{0x0b}, {}}}; }
Program function_ref(Index value) { return {{{0xd2}, value}, {{0x0b}, {}}}; }

constexpr std::string_view ctor_name = "__wasm_call_ctors";
constexpr std::string_view stack_name = "__stack_pointer";
constexpr std::string_view table_name = "__indirect_function_table";
constexpr GlobalType stack_type{Type::i32, true};

struct SymbolRef {
  std::size_t object;
  Index index;
  bool operator==(const SymbolRef&) const = default;
};

struct TypeLess {
  bool operator()(const FunctionType& lhs, const FunctionType& rhs) const {
    return std::tie(lhs.params, lhs.results) < std::tie(rhs.params, rhs.results);
  }
};

class Linker {
public:
  Linker(std::span<const ObjectLinkInput> inputs, const ObjectLinkOptions& options)
      : objects_(inputs.size()), options_(options) {
    for (std::size_t m = 0; m < inputs.size(); ++m)
      objects_[m].source = inputs[m];
  }

  Module run() {
    for_each_object([&](std::size_t, Object& object) { read_object(object); });
    check_features();
    collect_symbols();
    layout_data();
    allocate_indices();
    collect_elements();
    append_definitions();
    append_ctors();
    append_exports();
    finish_storage();
    validate_module(output_);
    return std::move(output_);
  }

private:
  std::vector<Object> objects_;
  const ObjectLinkOptions& options_;
  Module output_;
  std::map<FunctionType, Index, TypeLess> types_;
  std::map<std::string_view, SymbolRef> definitions_;
  std::array<Index, entity_kinds.size()> counts_{};
  Index imported_globals_{};
  std::optional<Index> stack_, ctor_;
  bool needs_stack_{}, needs_ctors_{}, needs_memory_{}, needs_table_{};
  Limits memory_limits_, table_limits_;
  Index stack_top_{};
  std::map<Index, Index> table_slots_;
  std::vector<Program> table_init_, declarations_;
  std::map<std::string, std::pair<Kind, Index>, std::less<>> exports_;

  template<class Run>
  void for_each_object(Run run) {
    for (std::size_t m = 0; m < objects_.size(); ++m) {
      try {
        run(m, objects_[m]);
      } catch (const Error& error) {
        auto message = std::string_view(error.what());
        const auto prefix = "at byte offset " + std::to_string(error.offset) + ": ";
        if (message.starts_with(prefix))
          message.remove_prefix(prefix.size());
        throw Error(error.offset, "Wasm object " + object_name(m) + ": " + std::string(message));
      }
    }
  }

  std::string object_name(std::size_t m) const {
    const auto name = objects_[m].source.name;
    return name.empty() ? "#" + std::to_string(m) : "'" + std::string(name) + "'";
  }

  Index intern_type(const FunctionType& type) {
    const auto [it, inserted] = types_.try_emplace(type, index_of(output_.types.size()));
    if (inserted)
      output_.types.push_back(type);
    return it->second;
  }

  void check_features() {
    std::map<std::string_view, std::pair<bool, std::size_t>> seen;
    constexpr std::array<std::string_view, 9> supported{"bulk-memory", "bulk-memory-opt",
        "call-indirect-overlong", "multivalue", "mutable-globals", "nontrapping-fptoint",
        "reference-types", "sign-ext", "simd128"};
    for_each_object([&](std::size_t m, Object& object) {
      for (const auto& [name, feature] : object.features) {
        const auto [used, at] = feature;
        if (used && std::find(supported.begin(), supported.end(), name) == supported.end())
          bad(at, "unsupported Wasm target feature '+" + name + "'");
        const auto [it, inserted] = seen.try_emplace(name, used, m);
        if (!inserted && used != it->second.first)
          bad(at, "conflicting target feature '" + name + "' with " + object_name(it->second.second));
      }
    });
  }

  const Symbol& symbol(SymbolRef ref) const {
    return objects_[ref.object].symbols[ref.index];
  }

  std::optional<SymbolRef> winner(SymbolRef ref) const {
    const auto& sym = symbol(ref);
    if (sym.is_local())
      return ref;
    const auto it = definitions_.find(sym.name);
    if (it == definitions_.end())
      return std::nullopt;
    const auto& def = symbol(it->second);
    if (def.kind != sym.kind)
      bad(sym.at, "symbol '" + sym.name + "' has a different kind in " +
                  object_name(it->second.object));
    return it->second;
  }

  void collect_symbols() {
    for_each_object([&](std::size_t m, Object& object) {
      std::set<std::string_view> names;
      for (std::size_t s = 0; s < object.symbols.size(); ++s) {
        const auto& sym = object.symbols[s];
        if (sym.is_local())
          continue;
        if (!names.insert(sym.name).second)
          bad(sym.at, "duplicate non-local symbol '" + sym.name + "' in symbol table");
        if (sym.is_undefined())
          continue;
        if (sym.name == ctor_name)
          bad(sym.at, "reserved synthetic symbol '" + sym.name + "' cannot be defined");
        const SymbolRef ref{m, index_of(s)};
        const auto [it, inserted] = definitions_.emplace(sym.name, ref);
        if (inserted)
          continue;
        const auto& prior = symbol(it->second);
        if (prior.kind != sym.kind || (!prior.is_weak() && !sym.is_weak()))
          bad(sym.at, "duplicate definition of symbol '" + sym.name + "' (first in " +
                      object_name(it->second.object) + ")");
        if (prior.is_weak() && !sym.is_weak())
          it->second = ref;
      }
    });
  }

  void layout_data() {
    std::uint64_t next = 1024; // Reserve low addresses, including the C null pointer.
    for_each_object([&](std::size_t, Object& object) {
      for (std::size_t i = 0; i < object.data.size(); ++i) {
        needs_memory_ = true;
        auto& data = object.data[i];
        const auto align = (std::uint64_t{1} << data.align) - 1;
        next = (next + align) & ~align;
        data.address = index_of(next, data.at);
        next += object.module.data[i].bytes.size();
        index_of(next, data.at);
      }
    });
    stack_top_ = index_of(next);
  }

  static void require_limits(Limits& combined, const Limits& input, std::string_view what) {
    combined.min = std::max(combined.min, input.min);
    if (input.max && (!combined.max || *input.max < *combined.max))
      combined.max = input.max;
    if (combined.max && combined.min > *combined.max)
      bad(0, "incompatible " + std::string(what) + " limits");
  }

  bool is_builtin(const Entity& entity, const Symbol* sym, std::string_view name) const {
    return entity.import->module == "env" && entity.import->name == name &&
           (!sym || sym->name == name);
  }

  ImportDesc descriptor(const Object& object, Kind kind, Index index) const {
    if (const auto* import = object.entities[space(kind)][index].import) {
      if (kind == Kind::function)
        return object.types[std::get<Index>(import->desc)];
      return import->desc;
    }
    const auto def = index - object.imported[space(kind)];
    if (kind == Kind::function)
      return object.types[object.module.functions[def].type];
    return object.module.globals[def].type;
  }

  void allocate_indices() {
    for_each_object([&](std::size_t, Object& object) {
      for (const auto& type : object.module.types)
        object.types.push_back(intern_type(type));
      for (const auto& import : object.module.imports)
        if (const auto* type = std::get_if<Index>(&import.desc); type && *type >= object.types.size())
          bad(0, "function import '" + import.name + "' has an out-of-range type index");
      for (const auto& fn : object.module.functions)
        if (fn.type >= object.types.size())
          bad(fn.body.offset, "function has an out-of-range type index");
    });
    const bool requested_ctors = std::find(options_.exports.begin(), options_.exports.end(),
                                          ctor_name) != options_.exports.end();
    needs_ctors_ = requested_ctors;
    std::map<std::pair<std::string_view, std::string_view>, std::pair<std::size_t, Index>> host;
    for_each_object([&](std::size_t m, Object& object) {
      needs_ctors_ |= !object.ctors.empty();
      std::array<Index, entity_kinds.size()> indices{};
      for (const auto& import : object.module.imports) {
        const auto k = import.desc.index();
        const auto kind = static_cast<Kind>(k);
        const auto index = indices[k]++;
        auto& entity = object.entities[k][index];
        const auto* sym = entity.symbol ? &object.symbols[*entity.symbol] : nullptr;
        const auto at = sym ? sym->at : 0;
        if (kind == Kind::memory) {
          if (!is_builtin(entity, sym, "__linear_memory"))
            bad(at, "unsupported object memory import '" + import.module + "." + import.name + "'");
          require_limits(memory_limits_, std::get<MemoryType>(import.desc).limits, "object memory");
          needs_memory_ = true;
          entity.output = 0;
          continue;
        }
        if (kind == Kind::table) {
          const auto& table = std::get<TableType>(import.desc);
          if (!is_builtin(entity, sym, table_name) || table.element != Type::funcref)
            bad(at, "unsupported object table import '" + import.module + "." + import.name + "'");
          require_limits(table_limits_, table.limits, "object table");
          needs_table_ = true;
          entity.output = 0;
          continue;
        }
        const auto desc = descriptor(object, kind, index);
        if (entity.symbol) {
          if (const auto provider = winner({m, *entity.symbol})) {
            const auto& def = symbol(*provider);
            if (desc != descriptor(objects_[provider->object], kind, def.index))
              bad(at, "incompatible type for symbol '" + sym->name + "' defined in " +
                      object_name(provider->object));
            continue;
          }
        }
        if (kind == Kind::global && is_builtin(entity, sym, stack_name)) {
          if (std::get<GlobalType>(desc) != stack_type)
            bad(at, "__stack_pointer import must be a mutable i32 global");
          needs_stack_ = true;
          continue;
        }
        if (kind == Kind::function && is_builtin(entity, sym, ctor_name)) {
          if (output_.types[std::get<Index>(desc)] != FunctionType{})
            bad(at, "__wasm_call_ctors import must have type () -> ()");
          needs_ctors_ = true;
          continue;
        }
        if (sym && sym->is_weak())
          continue;
        const auto [it, inserted] = host.try_emplace(std::pair{std::string_view(import.module),
              std::string_view(import.name)}, output_.imports.size(), counts_[k]);
        if (inserted) {
          output_.imports.push_back({import.module, import.name, desc});
          ++counts_[k];
        } else if (output_.imports[it->second.first].desc != desc) {
          bad(at, "incompatible host imports '" + import.module + "." + import.name + "'");
        }
        entity.output = it->second.second;
      }
    });
    imported_globals_ = counts_[space(Kind::global)];
    if (needs_stack_) {
      needs_memory_ = true;
      stack_top_ = index_of(((std::uint64_t(stack_top_) + 15) & ~std::uint64_t{15}) + 65536);
      stack_ = counts_[space(Kind::global)]++;
      output_.globals.push_back({stack_type, constant(stack_top_)});
    }
    for_each_object([&](std::size_t m, Object& object) {
      for (const auto k : {space(Kind::function), space(Kind::global)}) {
        auto& entities = object.entities[k];
        for (std::size_t i = object.imported[k]; i < entities.size(); ++i) {
          auto& entity = entities[i];
          if (entity.symbol) {
            const SymbolRef ref{m, *entity.symbol};
            if (const auto def = winner(ref); def != ref) {
              if (descriptor(object, static_cast<Kind>(k), index_of(i)) !=
                  descriptor(objects_[def->object], static_cast<Kind>(k), symbol(*def).index))
                bad(symbol(ref).at, "incompatible type for weak definition '" + symbol(ref).name + "'");
              continue;
            }
          }
          entity.output = counts_[k]++;
        }
      }
    });
    if (needs_ctors_)
      ctor_ = counts_[space(Kind::function)]++;
    // All definitions (including synthesized ones) now have assigned indices.
    for_each_object([&](std::size_t m, Object& object) {
      for (const auto kind : {Kind::function, Kind::global}) {
        auto& entities = object.entities[space(kind)];
        for (auto& entity : entities) {
          if (entity.output)
            continue;
          const auto* sym = entity.symbol ? &object.symbols[*entity.symbol] : nullptr;
          if (entity.symbol) {
            if (const auto def = winner({m, *entity.symbol})) {
              entity.output = objects_[def->object].entities[space(kind)][symbol(*def).index].output;
              continue;
            }
          }
          if (entity.import && kind == Kind::global && is_builtin(entity, sym, stack_name))
            entity.output = stack_;
          if (entity.import && kind == Kind::function && is_builtin(entity, sym, ctor_name))
            entity.output = ctor_;
        }
      }
    });
  }

  const Entity& entity_at(const Object& object, Kind kind, Index index, std::size_t at) const {
    const auto& entities = object.entities[space(kind)];
    if (index >= entities.size())
      bad(at, "object instruction or metadata references an out-of-range index");
    return entities[index];
  }

  Index mapped(const Object& object, Kind kind, Index index, std::size_t at) const {
    const auto& entity = entity_at(object, kind, index, at);
    if (!entity.output)
      bad(at, "unresolved weak symbol '" + object.symbols[*entity.symbol].name + "'");
    return *entity.output;
  }

  Index table_slot(Index fn) {
    const auto [it, inserted] = table_slots_.try_emplace(fn, index_of(table_init_.size() + 1));
    if (inserted)
      table_init_.push_back(function_ref(fn));
    needs_table_ = true;
    return it->second;
  }

  void collect_elements() {
    for_each_object([&](std::size_t, Object& object) {
      for (const auto& element : object.module.elements) {
        if (element.type != Type::funcref || element.mode == SegmentMode::passive)
          bad(0, "only active or declarative function element segments are supported in objects");
        if (element.mode == SegmentMode::active) {
          if (element.table != 0 || element.offset.size() != 2 ||
              element.offset.front().opcode != Opcode{0x41} ||
              std::get<Index>(element.offset.front().immediate) != 1)
            bad(0, "object function elements must target table zero at i32.const 1");
          mapped(object, Kind::table, 0, element.offset.front().offset);
        }
        for (const auto& expr : element.init) {
          if (expr.size() != 2 || expr.front().opcode != Opcode{0xd2})
            bad(0, "object element must be a function reference");
          const auto& insn = expr.front();
          const auto& entity = entity_at(object, Kind::function, std::get<Index>(insn.immediate),
                                         insn.offset);
          if (!entity.output)
            continue; // A weak undefined function has a null table address.
          if (element.mode == SegmentMode::active)
            table_slot(*entity.output);
          else
            declarations_.push_back(function_ref(*entity.output));
        }
      }
    });
  }

  Index data_address(SymbolRef ref, const Fixup* fixup = nullptr) const {
    const auto at = fixup ? fixup->at : symbol(ref).at;
    const auto def = winner(ref);
    if (!def) {
      if (symbol(ref).is_weak() && fixup)
        return 0;
      bad(at, "undefined data symbol '" + symbol(ref).name + "'");
    }
    const auto& sym = symbol(*def);
    const auto sum = std::int64_t(objects_[def->object].data[sym.index].address) +
                     sym.offset + (fixup ? fixup->addend : 0);
    if (sum < 0 || sum > std::numeric_limits<Index>::max())
      bad(at, "wasm32 address overflow for data symbol '" + sym.name + "'");
    return static_cast<Index>(sum);
  }

  Index relocate(std::size_t m, const Fixup& fixup) {
    const auto& object = objects_[m];
    if (fixup.type == type_leb)
      return object.types[fixup.index];
    if (is_address(fixup.type))
      return data_address({m, fixup.index}, &fixup);
    const auto& sym = object.symbols[fixup.index];
    const auto kind = kind_of(sym.kind);
    if (fixup.type == slot_i32 || fixup.type == slot_sleb) {
      const auto fn = entity_at(object, kind, sym.index, fixup.at).output;
      return fn ? table_slot(*fn) : 0;
    }
    return mapped(object, kind, sym.index, fixup.at);
  }

  Index read_operand(std::size_t m, Reader& in, Reloc required, Reloc alternative,
                     Index original, bool emit, std::optional<Kind> fallback = {}) {
    auto& object = objects_[m];
    const auto at = in.offset();
    in.leb("object instruction operand", required == address_sleb);
    const auto it = object.code_fixups.find(at);
    if (it == object.code_fixups.end()) {
      if (!emit)
        return original;
      if (fallback)
        return mapped(object, *fallback, original, at);
      if (required == type_leb) {
        if (original >= object.types.size())
          bad(at, "object instruction type index is out of range");
        return object.types[original];
      }
      return original;
    }
    const auto& fixup = it->second;
    if (fixup.type != required && fixup.type != alternative)
      bad(fixup.at, relocation_name(fixup.type) + " does not match its CODE operand at byte " +
                    std::to_string(at));
    if (in.offset() - at != 5)
      bad(fixup.at, relocation_name(fixup.type) + " requires a 5-byte padded CODE operand");
    const auto result = emit ? relocate(m, fixup) : original;
    object.code_fixups.erase(it);
    return result;
  }

  void rewrite_program(std::size_t m, Program& program, bool emit) {
    auto& object = objects_[m];
    for (auto& insn : program) {
      Reader in(object.source.bytes.subspan(insn.offset + 1), insn.offset + 1);
      const auto opcode = insn.opcode;
      const auto operand = [&](Reloc reloc, Index& index, std::optional<Kind> kind = {}) {
        index = read_operand(m, in, reloc, reloc, index, emit, kind);
      };
      const auto index = [&](Reloc reloc, Kind kind) {
        operand(reloc, std::get<Index>(insn.immediate), kind);
      };
      if (opcode.byte == 0xfc || opcode.byte == 0xfd)
        in.leb("extended opcode");
      switch (opcode.byte) {
      case 0x02: case 0x03: case 0x04:
        if (auto* type = std::get_if<Index>(&std::get<BlockType>(insn.immediate).value))
          operand(type_leb, *type);
        break;
      case 0x10: case 0xd2: index(function_leb, Kind::function); break;
      case 0x11: {
        auto& pair = std::get<IndexPair>(insn.immediate);
        operand(type_leb, pair.first);
        operand(table_leb, pair.second, Kind::table);
        break;
      }
      case 0x23: case 0x24: index(global_leb, Kind::global); break;
      case 0x25: case 0x26: index(table_leb, Kind::table); break;
      case 0x41: {
        auto& value = std::get<Index>(insn.immediate);
        value = read_operand(m, in, address_sleb, slot_sleb, value, emit);
        break;
      }
      case 0xfc:
        switch (opcode.extension) {
        case 8: case 9: case 12: case 13:
          bad(insn.offset, "data/element index instructions are not supported in objects");
        case 14: {
          auto& pair = std::get<IndexPair>(insn.immediate);
          operand(table_leb, pair.first, Kind::table);
          operand(table_leb, pair.second, Kind::table);
          break;
        }
        case 15: case 16: case 17: index(table_leb, Kind::table); break;
        }
        break;
      }
      auto* memory = std::get_if<MemoryArg>(&insn.immediate);
      if (auto* lane = std::get_if<MemoryLane>(&insn.immediate))
        memory = &lane->memory;
      if (memory) {
        in.leb("memory argument alignment");
        operand(address_leb, memory->offset);
      }
    }
  }

  void rewrite_data(std::size_t m, Data& data, const DataInfo& info) {
    auto& object = objects_[m];
    const auto end = info.at + data.bytes.size();
    auto it = object.data_fixups.lower_bound(info.at);
    std::size_t prior_end = info.at;
    while (it != object.data_fixups.end() && it->first < end) {
      const auto [at, fixup] = *it;
      const auto width = is_word(fixup.type) ? 4u : 5u;
      if (width > end - at)
        bad(fixup.at, "DATA relocation extends beyond its segment payload");
      if (at < prior_end)
        bad(fixup.at, "overlapping DATA relocations");
      prior_end = at + width;
      auto value = relocate(m, fixup);
      const auto offset = at - info.at;
      if (is_word(fixup.type)) {
        for (unsigned byte = 0; byte < width; ++byte)
          data.bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
      } else {
        const bool sign = fixup.type == slot_sleb || fixup.type == address_sleb;
        Reader original(object.source.bytes.subspan(at, width), at);
        original.leb("DATA relocation operand", sign);
        if (!original.rest().empty())
          bad(fixup.at, relocation_name(fixup.type) + " requires a 5-byte padded DATA operand");
        for (unsigned byte = 0; byte < width - 1; ++byte) {
          data.bytes[offset + byte] = static_cast<std::uint8_t>(0x80 | (value & 0x7f));
          value >>= 7;
        }
        data.bytes[offset + width - 1] = static_cast<std::uint8_t>(
            sign && (value & 8) ? value | 0x70 : value);
      }
      it = object.data_fixups.erase(it);
    }
    data.offset = constant(info.address);
  }

  void append_definitions() {
    for_each_object([&](std::size_t m, Object& object) {
      auto& module = object.module;
      for (std::size_t i = 0; i < module.functions.size(); ++i) {
        // Check relocations in losing weak bodies too, but do not emit them.
        auto& fn = module.functions[i];
        const auto entity = object.entities[space(Kind::function)][object.imported[0] + i];
        const bool emit = !entity.symbol ||
                          winner({m, *entity.symbol}) == SymbolRef{m, *entity.symbol};
        rewrite_program(m, fn.body.instructions, emit);
        if (emit)
          output_.functions.push_back({object.types[fn.type], std::move(fn.body)});
      }
      if (!object.code_fixups.empty()) {
        const auto [at, fixup] = *object.code_fixups.begin();
        bad(fixup.at, relocation_name(fixup.type) + " does not target a supported CODE operand at byte " +
                      std::to_string(at));
      }
      for (std::size_t i = 0; i < module.globals.size(); ++i) {
        const auto entity = object.entities[space(Kind::global)][object.imported[3] + i];
        if (entity.symbol && winner({m, *entity.symbol}) != SymbolRef{m, *entity.symbol})
          continue;
        auto& global = module.globals[i];
        // Global initializers are synthetic sections, never CODE reloc targets.
        for (auto& insn : global.init) {
          if (insn.opcode == Opcode{0x23} || insn.opcode == Opcode{0xd2}) {
            const auto kind = insn.opcode.byte == 0x23 ? Kind::global : Kind::function;
            auto& index = std::get<Index>(insn.immediate);
            index = mapped(object, kind, index, insn.offset);
            if (kind == Kind::global && index >= imported_globals_)
              bad(insn.offset, "object initializer cannot reference a resolved global definition");
          }
        }
        output_.globals.push_back(std::move(global));
      }
      for (std::size_t i = 0; i < module.data.size(); ++i) {
        auto& data = module.data[i];
        rewrite_data(m, data, object.data[i]);
        output_.data.push_back(std::move(data));
      }
      if (!object.data_fixups.empty()) {
        const auto [at, fixup] = *object.data_fixups.begin();
        bad(fixup.at, relocation_name(fixup.type) + " does not lie within a DATA segment at byte " +
                      std::to_string(at));
      }
    });
  }

  void append_ctors() {
    if (!ctor_)
      return;
    std::vector<std::pair<Index, Index>> entries;
    for_each_object([&](std::size_t, Object& object) {
      for (const auto& [priority, index] : object.ctors) {
        const auto& sym = object.symbols[index];
        const auto type = std::get<Index>(descriptor(object, Kind::function, sym.index));
        if (output_.types[type] != FunctionType{})
          bad(sym.at, "constructor '" + sym.name + "' must have type () -> ()");
        entries.emplace_back(priority, mapped(object, Kind::function, sym.index, sym.at));
      }
    });
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    Program body;
    for (const auto& entry : entries)
      body.push_back({{0x10}, entry.second});
    body.push_back({{0x0b}, {}});
    output_.functions.push_back({intern_type({}), {0, {}, std::move(body)}});
  }

  void add_export(std::string_view name, Kind kind, Index index, std::size_t at = 0) {
    const auto [it, inserted] = exports_.try_emplace(std::string(name), kind, index);
    if (inserted)
      output_.exports.push_back({std::string(name), kind, index});
    else if (it->second != std::pair{kind, index})
      bad(at, "conflicting object exports named '" + std::string(name) + "'");
  }

  void export_symbol(SymbolRef ref) {
    const auto& sym = symbol(ref);
    if (sym.kind == SymKind::data) {
      if (!exports_.contains(sym.name)) {
        const auto index = counts_[space(Kind::global)]++;
        output_.globals.push_back({{Type::i32, false}, constant(data_address(ref))});
        add_export(sym.name, Kind::global, index, sym.at);
      } else {
        bad(sym.at, "conflicting object exports named '" + sym.name + "'");
      }
      return;
    }
    const auto kind = kind_of(sym.kind);
    add_export(sym.name, kind, mapped(objects_[ref.object], kind, sym.index, sym.at), sym.at);
  }

  void append_exports() {
    std::set<std::string_view> auto_data;
    for_each_object([&](std::size_t m, Object& object) {
      for (const auto& entry : object.module.exports)
        add_export(entry.name, entry.kind, mapped(object, entry.kind, entry.index, 0));
      for (std::size_t i = 0; i < object.symbols.size(); ++i) {
        const auto& sym = object.symbols[i];
        if (sym.is_local())
          continue;
        const SymbolRef ref{m, index_of(i)};
        if (sym.flags & exported) {
          if (sym.kind != SymKind::data || auto_data.insert(sym.name).second)
            export_symbol(ref);
        } else if (!(sym.flags & hidden) && sym.kind != SymKind::data && winner(ref) == ref) {
          export_symbol(ref);
        }
      }
    });
    std::set<std::string_view> requested;
    for (const auto name : options_.exports) {
      if (!requested.insert(name).second)
        continue;
      if (name == ctor_name) {
        add_export(name, Kind::function, *ctor_);
      } else if (name == "memory" && needs_memory_) {
        add_export(name, Kind::memory, 0);
      } else if (name == table_name && needs_table_) {
        add_export(name, Kind::table, 0);
      } else if (const auto it = definitions_.find(name); it != definitions_.end()) {
        if (!auto_data.contains(name))
          export_symbol(it->second);
      } else if (!exports_.contains(name)) {
        bad(0, "requested export '" + std::string(name) + "' is not a defined object symbol");
      }
    }
  }

  void finish_storage() {
    if (needs_memory_) {
      const auto pages = (std::uint64_t(stack_top_) + 65535) / 65536;
      require_limits(memory_limits_, {index_of(pages), {}}, "linked memory");
      output_.memories.push_back({memory_limits_});
      add_export("memory", Kind::memory, 0);
    }
    if (needs_table_) {
      require_limits(table_limits_, {index_of(table_init_.size() + 1), {}}, "linked table");
      output_.tables.push_back({Type::funcref, table_limits_});
    }
    if (!table_init_.empty())
      output_.elements.push_back({SegmentMode::active, 0, constant(1), Type::funcref,
                                  std::move(table_init_)});
    if (!declarations_.empty())
      output_.elements.push_back({SegmentMode::declarative, 0, {}, Type::funcref,
                                  std::move(declarations_)});
  }
};

} // namespace

Module link_objects(std::span<const ObjectLinkInput> inputs, const ObjectLinkOptions& options) {
  if (inputs.empty())
    bad(0, "object linking requires at least one relocatable Wasm object");
  return Linker(inputs, options).run();
}

} // namespace limestone::wasmutils::core
