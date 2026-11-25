#include "WASM-Module.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace limestone::wasmutils::core {
namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> header{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};

enum class Section : std::uint8_t {
  custom, type, import, function, table, memory, global, export_, start, element,
  code, data, data_count
};

unsigned section_order(Section section) {
  switch (section) {
  case Section::data_count: return 10;
  case Section::code: return 11;
  case Section::data: return 12;
  default: return static_cast<unsigned>(section);
  }
}

bool reference_type(Type type) {
  return type == Type::funcref || type == Type::externref;
}

bool value_type(Type type) {
  switch (type) {
  case Type::i32: case Type::i64: case Type::f32: case Type::f64: case Type::v128:
    return true;
  default:
    return reference_type(type);
  }
}

// Reject truncated sequences, overlong forms, surrogates and values outside
// the Unicode range. NUL is a valid code point in a Wasm name.
bool valid_utf8(std::string_view name) {
  for (std::size_t i = 0; i < name.size();) {
    const auto lead = static_cast<unsigned char>(name[i++]);
    if (lead <= 0x7f)
      continue;
    unsigned trailing, codepoint, minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
      trailing = 1;
      codepoint = lead & 0x1f;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      trailing = 2;
      codepoint = lead & 0x0f;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      trailing = 3;
      codepoint = lead & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (trailing > name.size() - i)
      return false;
    for (; trailing != 0; --trailing) {
      const auto next = static_cast<unsigned char>(name[i++]);
      if ((next & 0xc0) != 0x80)
        return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
      return false;
  }
  return true;
}

class Reader {
public:
  Reader(std::span<const std::uint8_t> bytes, std::size_t base)
      : bytes_(bytes), position_(base) {}

  bool empty() const { return bytes_.empty(); }
  std::size_t position() const { return position_; }

  std::span<const std::uint8_t> take(std::size_t size, std::string_view field) {
    if (size > bytes_.size())
      throw Error(position_, "truncated " + std::string(field));
    const auto result = bytes_.first(size);
    bytes_ = bytes_.subspan(size);
    position_ += size;
    return result;
  }

  std::uint8_t byte(std::string_view field) {
    return take(1, field).front();
  }

  std::uint32_t u32(std::string_view field) {
    const auto start = position_;
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
      const auto octet = byte(field);
      if (shift == 28 && octet > 0x0f)
        throw Error(start, "invalid u32 LEB128 for " + std::string(field));
      result |= std::uint32_t{octet & 0x7fu} << shift;
      if (octet < 0x80)
        return result;
    }
    throw Error(start, "invalid u32 LEB128 for " + std::string(field));
  }

  void expect(std::uint8_t value, std::string_view field) {
    const auto start = position_;
    if (byte(field) != value)
      throw Error(start, "unsupported or invalid " + std::string(field));
  }

  void expect_end(std::string_view field) const {
    if (!empty())
      throw Error(position_, "unexpected trailing bytes in " + std::string(field));
  }

  Reader payload(std::string_view field) {
    const auto size = u32(field);
    const auto start = position_;
    return Reader(take(size, field), start);
  }

  std::uint32_t count(std::string_view field, std::size_t minimum_size) {
    const auto start = position_;
    const auto size = u32(field);
    if (size > bytes_.size() / minimum_size)
      throw Error(start, "truncated " + std::string(field));
    return size;
  }

  template<class Read>
  auto vector(std::string_view field, std::size_t minimum_size, Read read) {
    const auto size = count(field, minimum_size);
    std::vector<decltype(read())> result;
    if (static_cast<std::size_t>(size) > result.max_size())
      throw Error(position_, std::string(field) + " is too large");
    for (std::uint32_t i = 0; i < size; ++i)
      result.push_back(read());
    return result;
  }

  std::string name() {
    const auto size = u32("name length");
    const auto start = position_;
    const auto bytes = take(size, "name");
    if (bytes.empty())
      return {};
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), size);
    if (!valid_utf8(text))
      throw Error(start, "invalid UTF-8 name");
    return std::string(text);
  }

  Bytes blob() {
    const auto size = u32("byte vector length");
    const auto bytes = take(size, "byte vector");
    return Bytes(bytes.begin(), bytes.end());
  }

  Bytes remainder() {
    const auto bytes = take(bytes_.size(), "custom section contents");
    return Bytes(bytes.begin(), bytes.end());
  }

  Type type(bool reference = false) {
    const auto start = position_;
    const auto result = static_cast<Type>(byte(reference ? "reference type" : "value type"));
    if (reference ? !reference_type(result) : !value_type(result))
      throw Error(start, reference ? "invalid reference type" : "invalid value type");
    return result;
  }

  Program expression() {
    std::size_t used = 0;
    auto result = decode_expression(bytes_, used, position_);
    take(used, "expression");
    return result;
  }

private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_;
};

Limits read_limits(Reader& in) {
  const auto start = in.position();
  switch (in.byte("limits flags")) {
  case 0: return {in.u32("minimum limit"), {}};
  case 1: return {in.u32("minimum limit"), in.u32("maximum limit")};
  default: throw Error(start, "unsupported or invalid limits flags");
  }
}

TableType read_table_type(Reader& in) { return {in.type(true), read_limits(in)}; }
MemoryType read_memory_type(Reader& in) { return {read_limits(in)}; }

GlobalType read_global_type(Reader& in) {
  const auto type = in.type();
  const auto start = in.position();
  const auto mutability = in.byte("global mutability");
  if (mutability > 1)
    throw Error(start, "invalid global mutability");
  return {type, mutability != 0};
}

ExternalKind read_external_kind(Reader& in) {
  const auto start = in.position();
  const auto kind = in.byte("external kind");
  if (kind > static_cast<std::uint8_t>(ExternalKind::global))
    throw Error(start, "unsupported or invalid external kind");
  return static_cast<ExternalKind>(kind);
}

ImportDesc read_import_desc(Reader& in) {
  switch (read_external_kind(in)) {
  case ExternalKind::function: return in.u32("import type index");
  case ExternalKind::table: return read_table_type(in);
  case ExternalKind::memory: return read_memory_type(in);
  case ExternalKind::global: return read_global_type(in);
  }
  throw Error(in.position(), "invalid import kind");
}

FunctionType read_function_type(Reader& in) {
  in.expect(0x60, "function type form");
  return {
    in.vector("parameter types", 1, [&] { return in.type(); }),
    in.vector("result types", 1, [&] { return in.type(); })
  };
}

Element read_element(Reader& in) {
  const auto start = in.position();
  const auto flags = in.u32("element segment flags");
  if (flags > 7)
    throw Error(start, "unsupported or invalid element segment flags");
  Element segment{};
  switch (flags & 3) {
  case 0: case 2:
    segment.mode = SegmentMode::active;
    if ((flags & 3) == 2)
      segment.table = in.u32("element table index");
    segment.offset = in.expression();
    break;
  case 1:
    segment.mode = SegmentMode::passive;
    break;
  case 3:
    segment.mode = SegmentMode::declarative;
    break;
  }
  if ((flags & 4) == 0) {
    if (flags != 0)
      in.expect(0, "element kind");
    segment.init = in.vector("element function indices", 1, [&] {
      const auto index_offset = in.position();
      const auto index = in.u32("element function index");
      return Program{{{0xd2}, index, index_offset}, {{0x0b}, {}, index_offset}};
    });
  } else {
    if (flags != 4)
      segment.type = in.type(true);
    segment.init = in.vector("element expressions", 1, [&] { return in.expression(); });
  }
  return segment;
}

Data read_data(Reader& in) {
  const auto start = in.position();
  const auto flags = in.u32("data segment flags");
  if (flags > 2)
    throw Error(start, "unsupported or invalid data segment flags");
  Data segment{};
  if (flags == 1) {
    segment.mode = SegmentMode::passive;
  } else {
    segment.mode = SegmentMode::active;
    if (flags == 2)
      segment.memory = in.u32("data memory index");
    segment.offset = in.expression();
  }
  segment.bytes = in.blob();
  return segment;
}

void add_local_count(std::uint32_t& total, std::uint32_t count, std::size_t offset) {
  if (count > std::numeric_limits<std::uint32_t>::max() - total)
    throw Error(offset, "function declares too many locals");
  total += count;
}

void check_data_count_usage(const Program& body, bool has_data_count) {
  if (has_data_count)
    return;
  for (const auto& instruction : body)
    if (instruction.opcode == Opcode{0xfc, 8} || instruction.opcode == Opcode{0xfc, 9})
      throw Error(instruction.offset, "data-index instruction requires a data count section");
}

std::vector<Function> read_functions(Reader& in, const std::vector<std::uint32_t>& types,
                                     bool has_data_count) {
  const auto start = in.position();
  const auto count = in.count("code body count", 3);
  if (count != types.size())
    throw Error(start, "function and code section counts differ");
  std::vector<Function> functions;
  if (types.size() > functions.max_size())
    throw Error(start, "too many functions");
  for (const auto type : types) {
    auto body = in.payload("function body");
    const auto offset = body.position();
    std::uint32_t local_count = 0;
    auto locals = body.vector("local declarations", 2, [&] {
      const auto run_offset = body.position();
      const auto count = body.u32("local count");
      add_local_count(local_count, count, run_offset);
      return LocalRun{count, body.type()};
    });
    auto instructions = body.expression();
    body.expect_end("function body");
    check_data_count_usage(instructions, has_data_count);
    functions.push_back({type, {offset, std::move(locals), std::move(instructions)}});
  }
  return functions;
}

class Writer {
public:
  Bytes bytes;

  void byte(std::uint8_t value) { bytes.push_back(value); }

  void u32(std::uint32_t value) {
    do {
      const auto octet = value & 0x7f;
      value >>= 7;
      byte(static_cast<std::uint8_t>(octet | (value == 0 ? 0 : 0x80)));
    } while (value != 0);
  }

  void size(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max())
      throw Error(0, "section, payload or vector exceeds its u32 length");
    u32(static_cast<std::uint32_t>(value));
  }

  template<class Range, class Write>
  void vector(const Range& values, Write write) {
    size(values.size());
    for (const auto& value : values)
      write(value);
  }

  template<class Range>
  void append(const Range& values) {
    bytes.insert(bytes.end(), values.begin(), values.end());
  }

  void name(std::string_view value) {
    if (!valid_utf8(value))
      throw Error(0, "invalid UTF-8 name");
    size(value.size());
    append(value);
  }

  void blob(const Bytes& value) {
    size(value.size());
    append(value);
  }

  void type(Type value, bool reference = false) {
    if (reference ? !reference_type(value) : !value_type(value))
      throw Error(0, reference ? "invalid reference type" : "invalid value type");
    byte(static_cast<std::uint8_t>(value));
  }

  void expression(const Program& program) {
    check_expression(program);
    append(encode(program));
  }

  template<class Write>
  void section(Section id, Write write) {
    Writer payload;
    write(payload);
    byte(static_cast<std::uint8_t>(id));
    blob(payload.bytes);
  }

  template<class Range, class Write>
  void vector_section(Section id, const Range& values, Write write) {
    if (!values.empty())
      section(id, [&](Writer& payload) {
        payload.vector(values, [&](const auto& value) { write(payload, value); });
      });
  }
};

void write_limits(Writer& out, const Limits& limits) {
  out.byte(limits.max ? 1 : 0);
  out.u32(limits.min);
  if (limits.max)
    out.u32(*limits.max);
}

void write_table_type(Writer& out, const TableType& type) {
  out.type(type.element, true);
  write_limits(out, type.limits);
}

void write_memory_type(Writer& out, const MemoryType& type) { write_limits(out, type.limits); }

void write_global_type(Writer& out, const GlobalType& type) {
  out.type(type.type);
  out.byte(type.mutable_ ? 1 : 0);
}

void write_import(Writer& out, const Import& import) {
  if (import.desc.valueless_by_exception())
    throw Error(0, "missing import descriptor");
  out.name(import.module);
  out.name(import.name);
  out.byte(static_cast<std::uint8_t>(import.desc.index()));
  switch (static_cast<ExternalKind>(import.desc.index())) {
  case ExternalKind::function: out.u32(std::get<std::uint32_t>(import.desc)); break;
  case ExternalKind::table: write_table_type(out, std::get<TableType>(import.desc)); break;
  case ExternalKind::memory: write_memory_type(out, std::get<MemoryType>(import.desc)); break;
  case ExternalKind::global: write_global_type(out, std::get<GlobalType>(import.desc)); break;
  }
}

void write_export(Writer& out, const Export& export_) {
  if (export_.kind > ExternalKind::global)
    throw Error(0, "invalid export kind");
  out.name(export_.name);
  out.byte(static_cast<std::uint8_t>(export_.kind));
  out.u32(export_.index);
}

bool is_legacy_element_init(const Program& init) {
  return init.size() == 2 && init[0].opcode == Opcode{0xd2} &&
      std::holds_alternative<std::uint32_t>(init[0].immediate) &&
      init[1].opcode == Opcode{0x0b} && std::holds_alternative<std::monostate>(init[1].immediate);
}

void write_element(Writer& out, const Element& segment) {
  unsigned flags;
  switch (segment.mode) {
  case SegmentMode::active:
    flags = segment.table == 0 && segment.type == Type::funcref ? 0 : 2;
    break;
  case SegmentMode::passive: flags = 1; break;
  case SegmentMode::declarative: flags = 3; break;
  default: throw Error(0, "invalid element segment mode");
  }
  if (segment.mode != SegmentMode::active && (segment.table != 0 || !segment.offset.empty()))
    throw Error(0, "inactive element segment carries an active table or offset");
  const bool legacy = segment.type == Type::funcref &&
      std::ranges::all_of(segment.init, is_legacy_element_init);
  out.u32(flags | (legacy ? 0u : 4u));
  if (segment.mode == SegmentMode::active) {
    if (flags == 2)
      out.u32(segment.table);
    out.expression(segment.offset);
  }
  if (flags != 0) {
    if (legacy)
      out.byte(0); // elemkind: funcref
    else
      out.type(segment.type, true);
  }
  out.vector(segment.init, [&](const Program& init) {
    if (legacy)
      out.u32(std::get<std::uint32_t>(init.front().immediate));
    else
      out.expression(init);
  });
}

void write_data(Writer& out, const Data& segment) {
  switch (segment.mode) {
  case SegmentMode::active:
    out.u32(segment.memory == 0 ? 0 : 2);
    if (segment.memory != 0)
      out.u32(segment.memory);
    out.expression(segment.offset);
    break;
  case SegmentMode::passive:
    if (segment.memory != 0 || !segment.offset.empty())
      throw Error(0, "passive data segment carries an active memory or offset");
    out.u32(1);
    break;
  default:
    throw Error(0, "invalid data segment mode");
  }
  out.blob(segment.bytes);
}

void write_function_body(Writer& out, const FunctionBody& body, bool has_data_count) {
  Writer payload;
  std::uint32_t local_count = 0;
  payload.vector(body.locals, [&](const LocalRun& run) {
    add_local_count(local_count, run.count, body.offset);
    payload.u32(run.count);
    payload.type(run.type);
  });
  check_data_count_usage(body.instructions, has_data_count);
  payload.expression(body.instructions);
  out.blob(payload.bytes);
}

} // namespace

Module decode_module(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < header.size() || !std::ranges::equal(header, bytes.first(header.size())))
    throw Error(0, "expected a WebAssembly binary module (magic and version 1)");

  Reader in(bytes.subspan(header.size()), header.size());
  Module module;
  std::vector<std::uint32_t> function_types;
  std::array<bool, 13> seen{};
  unsigned last_order = 0;
  std::size_t data_count_offset = 0;
  while (!in.empty()) {
    const auto offset = in.position();
    const auto id = in.byte("section ID");
    if (id > static_cast<std::uint8_t>(Section::data_count))
      throw Error(offset, "unsupported Core 2.0 section ID " + std::to_string(id));
    const auto section = static_cast<Section>(id);
    if (section != Section::custom) {
      if (seen[id])
        throw Error(offset, "duplicate Core 2.0 section ID " + std::to_string(id));
      seen[id] = true;
      const auto order = section_order(section);
      if (order < last_order)
        throw Error(offset, "out-of-order Core 2.0 section ID " + std::to_string(id));
      last_order = order;
    }

    auto payload = in.payload("module section");
    switch (section) {
    case Section::custom:
      module.custom.push_back({payload.name(), payload.remainder()});
      break;
    case Section::type:
      module.types = payload.vector("type section entries", 3, [&] { return read_function_type(payload); });
      break;
    case Section::import:
      module.imports = payload.vector("import section entries", 4, [&] {
        return Import{payload.name(), payload.name(), read_import_desc(payload)};
      });
      break;
    case Section::function:
      function_types = payload.vector("function section entries", 1, [&] {
        return payload.u32("function type index");
      });
      break;
    case Section::table:
      module.tables = payload.vector("table section entries", 3, [&] { return read_table_type(payload); });
      break;
    case Section::memory:
      module.memories = payload.vector("memory section entries", 2, [&] { return read_memory_type(payload); });
      break;
    case Section::global:
      module.globals = payload.vector("global section entries", 3, [&] {
        return Global{read_global_type(payload), payload.expression()};
      });
      break;
    case Section::export_:
      module.exports = payload.vector("export section entries", 3, [&] {
        return Export{payload.name(), read_external_kind(payload), payload.u32("export index")};
      });
      break;
    case Section::start:
      module.start = payload.u32("start function index");
      break;
    case Section::element:
      module.elements = payload.vector("element section entries", 3, [&] { return read_element(payload); });
      break;
    case Section::code:
      module.functions = read_functions(payload, function_types, module.data_count.has_value());
      break;
    case Section::data:
      module.data = payload.vector("data section entries", 2, [&] { return read_data(payload); });
      break;
    case Section::data_count:
      data_count_offset = payload.position();
      module.data_count = payload.u32("data count");
      break;
    }
    payload.expect_end("module section");
  }
  if (function_types.size() != module.functions.size())
    throw Error(in.position(), "function and code section counts differ");
  if (module.data_count && *module.data_count != module.data.size())
    throw Error(data_count_offset, "data count does not match the data section");
  return module;
}

Bytes encode_module(const Module& module) {
  if (module.data_count && *module.data_count != module.data.size())
    throw Error(0, "data count does not match the data section");
  Writer out;
  out.append(header);
  out.vector_section(Section::type, module.types, [](Writer& payload, const FunctionType& type) {
    payload.byte(0x60);
    payload.vector(type.params, [&](Type value) { payload.type(value); });
    payload.vector(type.results, [&](Type value) { payload.type(value); });
  });
  out.vector_section(Section::import, module.imports, write_import);
  out.vector_section(Section::function, module.functions, [](Writer& payload, const Function& function) {
    payload.u32(function.type);
  });
  out.vector_section(Section::table, module.tables, write_table_type);
  out.vector_section(Section::memory, module.memories, write_memory_type);
  out.vector_section(Section::global, module.globals, [](Writer& payload, const Global& global) {
    write_global_type(payload, global.type);
    payload.expression(global.init);
  });
  out.vector_section(Section::export_, module.exports, write_export);
  if (module.start)
    out.section(Section::start, [&](Writer& payload) { payload.u32(*module.start); });
  out.vector_section(Section::element, module.elements, write_element);
  if (module.data_count)
    out.section(Section::data_count, [&](Writer& payload) { payload.u32(*module.data_count); });
  out.vector_section(Section::code, module.functions, [&](Writer& payload, const Function& function) {
    write_function_body(payload, function.body, module.data_count.has_value());
  });
  out.vector_section(Section::data, module.data, write_data);
  for (const auto& custom : module.custom)
    out.section(Section::custom, [&](Writer& payload) {
      payload.name(custom.name);
      payload.append(custom.bytes);
    });
  return std::move(out.bytes);
}

} // namespace limestone::wasmutils::core
