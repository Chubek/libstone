#include "WASM-Core.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace limestone::wasmutils::core {
namespace {

constexpr bool is_prefix(std::uint8_t byte) {
  return byte == 0xfc || byte == 0xfd;
}

const OpcodeInfo& require_opcode(Opcode opcode, std::size_t offset) {
  if (const auto* info = lookup(opcode))
    return *info;
  constexpr std::string_view hex = "0123456789abcdef";
  std::string message = "unknown Core 2.0 opcode 0x";
  message += hex[opcode.byte >> 4];
  message += hex[opcode.byte & 0x0f];
  if (is_prefix(opcode.byte) || opcode.extension != 0)
    message += " with extension " + std::to_string(opcode.extension);
  throw Error(offset, message);
}

constexpr bool is_ref_type(Type type) {
  return type == Type::funcref || type == Type::externref;
}

constexpr bool is_value_type(Type type) {
  return type == Type::i32 || type == Type::i64 || type == Type::f32 ||
      type == Type::f64 || type == Type::v128 || is_ref_type(type);
}

class Decoder {
public:
  explicit Decoder(std::span<const std::uint8_t> bytes, std::size_t base = 0)
      : bytes_(bytes), base_(base), start_(base) {}

  bool empty() const { return cursor_ == bytes_.size(); }
  std::size_t position() const { return base_ + cursor_; }

  Instruction instruction() {
    start_ = position();
    Opcode opcode{byte("opcode")};
    if (is_prefix(opcode.byte))
      opcode.extension = u32();
    const auto& info = require_opcode(opcode, start_);
    return {opcode, immediate(info.format), start_};
  }

  std::uint8_t section_id() {
    start_ = position();
    return byte("section ID");
  }

  Decoder sized_payload(std::string_view what) {
    start_ = position();
    const auto size = u32();
    if (size > bytes_.size() - cursor_)
      fail("truncated " + std::string(what));
    Decoder payload(bytes_.subspan(cursor_, size), position());
    cursor_ += size;
    return payload;
  }

  std::uint32_t function_count() {
    start_ = position();
    const auto count = u32();
    // A function needs at least a size, a count of local runs, and an end.
    if (count > (bytes_.size() - cursor_) / 3)
      fail("truncated function bodies");
    return count;
  }

  std::vector<LocalRun> local_runs() {
    start_ = position();
    return vector<LocalRun>("local declarations", 1, [this] {
      start_ = position();
      return LocalRun{u32(), type()};
    });
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t base_;
  std::size_t cursor_{};
  std::size_t start_{}; // Report malformed operands at their instruction.

  [[noreturn]] void fail(const std::string& message) const {
    throw Error(start_, message);
  }

  std::uint8_t byte(std::string_view field) {
    if (empty())
      fail("truncated " + std::string(field));
    return bytes_[cursor_++];
  }

  // Section 5.2.2: allow padding up to ceil(Bits/7) bytes. At termination
  // any payload bits beyond Bits must match the sign (or be zero for u32).
  // Return the signed cases as sign-extended two's-complement bits.
  template<unsigned Bits, bool Signed>
  std::uint64_t leb() {
    constexpr std::string_view field = Signed ? "signed LEB128" : "unsigned LEB128";
    std::uint64_t result = 0;
    for (unsigned shift = 0;; shift += 7) {
      const unsigned octet = byte(field);
      const unsigned payload = octet & 0x7f;
      const unsigned used = std::min(Bits - shift, 7u);
      if ((octet & 0x80) != 0) {
        if (Bits - shift <= 7)
          fail(std::string(field) + " exceeds its maximum length");
        result |= std::uint64_t{payload} << shift;
        continue;
      }
      const unsigned mask = (1u << used) - 1;
      const bool negative = Signed && (payload & (1u << (used - 1))) != 0;
      const unsigned unused = 0x7f & ~mask;
      if ((payload & unused) != (negative ? unused : 0u))
        fail(std::string(field) + " has invalid terminal bits");
      result |= std::uint64_t{payload & mask} << shift;
      if (negative && shift + used < 64)
        result |= ~std::uint64_t{0} << (shift + used);
      return result;
    }
  }

  std::uint32_t u32() { return static_cast<std::uint32_t>(leb<32, false>()); }

  template<class UInt>
  UInt little_endian() {
    UInt result = 0;
    for (unsigned shift = 0; shift < std::numeric_limits<UInt>::digits; shift += 8)
      result |= static_cast<UInt>(byte("floating-point immediate")) << shift;
    return result;
  }

  void zero_byte() {
    if (byte("reserved zero byte") != 0)
      fail("expected a literal reserved zero byte");
  }

  Type type(bool reference = false) {
    const auto value = static_cast<Type>(byte(reference ? "reference type" : "value type"));
    if (reference ? !is_ref_type(value) : !is_value_type(value))
      fail(reference ? "invalid reference type" : "invalid value type");
    return value;
  }

  BlockType block_type() {
    if (empty())
      fail("truncated block type");
    if (bytes_[cursor_] == 0x40) {
      ++cursor_;
      return {};
    }
    const auto value = static_cast<Type>(bytes_[cursor_]);
    if (is_value_type(value)) {
      ++cursor_;
      return {value};
    }
    const auto index = leb<33, true>();
    if (index > std::numeric_limits<std::uint32_t>::max())
      fail("negative block type index");
    return {static_cast<std::uint32_t>(index)};
  }

  template<class T, class ReadElement>
  std::vector<T> vector(std::string_view field, std::size_t trailing, ReadElement read) {
    const auto count = u32();
    const auto remaining = bytes_.size() - cursor_;
    // Each element needs at least one byte, and br_table also needs a
    // default label. Grow only for elements actually read.
    if (remaining < trailing || count > remaining - trailing)
      fail("truncated " + std::string(field));
    std::vector<T> result;
    if (count > result.max_size())
      fail(std::string(field) + " is too large");
    for (std::uint32_t i = 0; i < count; ++i)
      result.push_back(read());
    return result;
  }

  MemoryArg memory() { return {u32(), u32()}; }

  Immediate immediate(Format format) {
    switch (format) {
    case Format::none:
      return std::monostate{};
    case Format::block:
      return block_type();
    case Format::index:
      return u32();
    case Format::index_pair:
      return IndexPair{u32(), u32()};
    case Format::branch_table:
      return BranchTable{vector<std::uint32_t>("branch table", 1, [this] { return u32(); }),
                         u32()};
    case Format::types:
      return vector<Type>("select type vector", 0, [this] { return type(); });
    case Format::ref_type:
      return type(true);
    case Format::memory:
      return memory();
    case Format::memory_lane:
      return MemoryLane{memory(), byte("lane index")};
    case Format::lane:
      return byte("lane index");
    case Format::zero:
      zero_byte();
      return std::monostate{};
    case Format::index_zero: {
      const auto index = u32();
      zero_byte();
      return index;
    }
    case Format::zero_zero:
      zero_byte();
      zero_byte();
      return std::monostate{};
    case Format::i32:
      return static_cast<std::uint32_t>(leb<32, true>());
    case Format::i64:
      return leb<64, true>();
    case Format::f32:
      return little_endian<std::uint32_t>();
    case Format::f64:
      return little_endian<std::uint64_t>();
    case Format::vector_bytes: {
      VectorBytes result;
      for (auto& octet : result)
        octet = byte("16-byte SIMD immediate");
      return result;
    }
    }
    fail("unknown immediate format");
  }
};

using Bytes = std::vector<std::uint8_t>;

void write_u32(Bytes& bytes, std::uint32_t value) {
  do {
    const auto payload = value & 0x7f;
    value >>= 7;
    bytes.push_back(static_cast<std::uint8_t>(payload | (value != 0 ? 0x80u : 0)));
  } while (value != 0);
}

void write_signed(Bytes& bytes, std::int64_t value) {
  for (;;) {
    const auto payload = static_cast<std::uint8_t>(value & 0x7f);
    value >>= 7; // Arithmetic shift (C++20).
    const bool sign = (payload & 0x40) != 0;
    if (value == (sign ? -1 : 0)) {
      bytes.push_back(payload);
      return;
    }
    bytes.push_back(static_cast<std::uint8_t>(payload | 0x80));
  }
}

template<class UInt>
void write_little_endian(Bytes& bytes, UInt value) {
  for (unsigned shift = 0; shift < std::numeric_limits<UInt>::digits; shift += 8)
    bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void write_memory(Bytes& bytes, const MemoryArg& memory) {
  write_u32(bytes, memory.align);
  write_u32(bytes, memory.offset);
}

class Encoder {
public:
  Encoder(Bytes& bytes, const Instruction& instruction)
      : bytes_(bytes), instruction_(instruction),
        info_(require_opcode(instruction.opcode, instruction.offset)) {}

  void write() {
    const auto& opcode = instruction_.opcode;
    bytes_.push_back(opcode.byte);
    if (is_prefix(opcode.byte))
      write_u32(bytes_, opcode.extension);

    switch (info_.format) {
    case Format::none:
      operand<std::monostate>();
      break;
    case Format::block:
      write_block_type(operand<BlockType>());
      break;
    case Format::index:
      write_u32(bytes_, operand<std::uint32_t>());
      break;
    case Format::index_pair: {
      const auto& indices = operand<IndexPair>();
      write_u32(bytes_, indices.first);
      write_u32(bytes_, indices.second);
      break;
    }
    case Format::branch_table: {
      const auto& table = operand<BranchTable>();
      write_count(table.labels.size());
      for (const auto label : table.labels)
        write_u32(bytes_, label);
      write_u32(bytes_, table.otherwise);
      break;
    }
    case Format::types: {
      const auto& types = operand<std::vector<Type>>();
      write_count(types.size());
      for (const auto type : types)
        write_type(type);
      break;
    }
    case Format::ref_type:
      write_type(operand<Type>(), true);
      break;
    case Format::memory:
      write_memory(bytes_, operand<MemoryArg>());
      break;
    case Format::memory_lane: {
      const auto& lane = operand<MemoryLane>();
      write_memory(bytes_, lane.memory);
      bytes_.push_back(lane.lane);
      break;
    }
    case Format::lane:
      bytes_.push_back(operand<std::uint8_t>());
      break;
    case Format::zero:
      operand<std::monostate>();
      bytes_.push_back(0);
      break;
    case Format::index_zero:
      write_u32(bytes_, operand<std::uint32_t>());
      bytes_.push_back(0);
      break;
    case Format::zero_zero:
      operand<std::monostate>();
      bytes_.push_back(0);
      bytes_.push_back(0);
      break;
    case Format::i32:
      write_signed(bytes_, std::bit_cast<std::int32_t>(operand<std::uint32_t>()));
      break;
    case Format::i64:
      write_signed(bytes_, std::bit_cast<std::int64_t>(operand<std::uint64_t>()));
      break;
    case Format::f32:
      write_little_endian(bytes_, operand<std::uint32_t>());
      break;
    case Format::f64:
      write_little_endian(bytes_, operand<std::uint64_t>());
      break;
    case Format::vector_bytes: {
      const auto& payload = operand<VectorBytes>();
      bytes_.insert(bytes_.end(), payload.begin(), payload.end());
      break;
    }
    }
  }

private:
  Bytes& bytes_;
  const Instruction& instruction_;
  const OpcodeInfo& info_;

  [[noreturn]] void fail(std::string_view message) const {
    throw Error(instruction_.offset, std::string(info_.mnemonic) + ": " +
        std::string(message));
  }

  template<class T>
  const T& operand() const {
    if (const auto* value = std::get_if<T>(&instruction_.immediate))
      return *value;
    fail("unexpected immediate type");
  }

  void write_count(std::size_t count) {
    if (count > std::numeric_limits<std::uint32_t>::max())
      fail("too many vector elements");
    write_u32(bytes_, static_cast<std::uint32_t>(count));
  }

  void write_type(Type type, bool reference = false) {
    if (reference ? !is_ref_type(type) : !is_value_type(type))
      fail(reference ? "invalid reference type" : "invalid value type");
    bytes_.push_back(static_cast<std::uint8_t>(type));
  }

  void write_block_type(const BlockType& block) {
    if (std::holds_alternative<std::monostate>(block.value))
      bytes_.push_back(0x40);
    else if (const auto* type = std::get_if<Type>(&block.value))
      write_type(*type);
    else if (const auto* index = std::get_if<std::uint32_t>(&block.value))
      write_signed(bytes_, *index);
    else
      fail("invalid block type");
  }
};

class ExpressionNesting {
public:
  // Returns true only for the expression's terminal end.
  bool add(const Instruction& instruction) {
    const auto opcode = instruction.opcode;
    if (opcode == Opcode{0x02} || opcode == Opcode{0x03})
      nesting_.push_back({Control::block_or_loop, instruction.offset});
    else if (opcode == Opcode{0x04})
      nesting_.push_back({Control::if_then, instruction.offset});
    else if (opcode == Opcode{0x05}) {
      if (nesting_.empty() || nesting_.back().control == Control::block_or_loop)
        throw Error(instruction.offset, "else without matching if");
      if (nesting_.back().control == Control::if_else)
        throw Error(instruction.offset, "duplicate else");
      nesting_.back().control = Control::if_else;
    } else if (opcode == Opcode{0x0b}) {
      if (nesting_.empty())
        return true;
      nesting_.pop_back();
    }
    return false;
  }

  [[noreturn]] void missing_end(std::size_t offset) const {
    if (!nesting_.empty())
      throw Error(nesting_.back().offset, "unterminated block, loop, or if");
    throw Error(offset, "missing expression end");
  }

private:
  enum class Control { block_or_loop, if_then, if_else };
  struct Frame {
    Control control;
    std::size_t offset;
  };
  std::vector<Frame> nesting_;
};

} // namespace

Program decode(std::span<const std::uint8_t> bytes) {
  Program result;
  Decoder decoder(bytes);
  while (!decoder.empty())
    result.push_back(decoder.instruction());
  return result;
}

Program decode_expression(std::span<const std::uint8_t> bytes, std::size_t& consumed,
                          std::size_t base_offset) {
  if (bytes.size() > std::numeric_limits<std::size_t>::max() - base_offset)
    throw Error(base_offset, "expression offsets exceed representable range");
  Decoder decoder(bytes, base_offset);
  Program result;
  ExpressionNesting nesting;
  while (!decoder.empty()) {
    result.push_back(decoder.instruction());
    if (nesting.add(result.back())) {
      consumed = decoder.position() - base_offset;
      return result;
    }
  }
  nesting.missing_end(decoder.position());
}

std::vector<std::uint8_t> encode(std::span<const Instruction> instructions) {
  Bytes result;
  for (const auto& instruction : instructions)
    Encoder(result, instruction).write();
  return result;
}

void check_expression(std::span<const Instruction> instructions) {
  ExpressionNesting nesting;
  for (std::size_t i = 0; i < instructions.size(); ++i) {
    if (nesting.add(instructions[i])) {
      if (i + 1 != instructions.size())
        throw Error(instructions[i + 1].offset, "unexpected instruction after expression end");
      return;
    }
  }
  nesting.missing_end(instructions.empty() ? 0 : instructions.back().offset);
}

std::vector<FunctionBody> decode_code_section(std::span<const std::uint8_t> module) {
  constexpr std::array<std::uint8_t, 8> header{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
  if (module.size() < header.size() || !std::ranges::equal(header, module.first(header.size())))
    throw Error(0, "expected a WebAssembly binary module (magic and version 1)");
  Decoder sections(module.subspan(header.size()), header.size());
  std::vector<FunctionBody> functions;
  bool has_code = false;
  while (!sections.empty()) {
    const auto offset = sections.position();
    const auto section = sections.section_id();
    if (section > 12)
      throw Error(offset, "unknown Core 2.0 section ID " + std::to_string(section));
    auto payload = sections.sized_payload("module section");
    if (section != 10)
      continue;
    if (has_code)
      throw Error(offset, "duplicate code section");
    has_code = true;
    const auto count = payload.function_count();
    for (std::uint32_t i = 0; i < count; ++i) {
      auto body = payload.sized_payload("function body");
      const auto start = body.position();
      FunctionBody function{start, body.local_runs(), {}};
      if (body.empty())
        throw Error(body.position(), "missing function expression");
      while (!body.empty())
        function.instructions.push_back(body.instruction());
      check_expression(function.instructions);
      functions.push_back(std::move(function));
    }
    if (!payload.empty())
      throw Error(payload.position(), "unexpected bytes after code section bodies");
  }
  return functions;
}

} // namespace limestone::wasmutils::core
