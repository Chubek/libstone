#include "WASM-Validate.hpp"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace limestone::wasmutils::core {
namespace {

using enum Type;
using Types = std::span<const Type>;
using StackType = std::optional<Type>;

bool is_ref(Type type) {
  return type == funcref || type == externref;
}

bool is_numeric(Type type) {
  return type == i32 || type == i64 || type == f32 || type == f64 || type == v128;
}

std::string_view type_name(Type type) {
  switch (type) {
  case i32: return "i32";
  case i64: return "i64";
  case f32: return "f32";
  case f64: return "f64";
  case v128: return "v128";
  case funcref: return "funcref";
  case externref: return "externref";
  }
  return "invalid type";
}

void value_type(Type type, std::size_t offset) {
  if (!is_numeric(type) && !is_ref(type))
    throw Error(offset, "invalid value type");
}

template<class T>
const T& indexed(const std::vector<T>& entries, std::uint32_t index,
                 std::string_view space, std::size_t offset) {
  if (index >= entries.size())
    throw Error(offset, std::string(space) + " index out of range: " + std::to_string(index));
  return entries[index];
}

[[noreturn]] void fail(const Instruction& instruction, std::string_view message) {
  const auto* info = lookup(instruction.opcode);
  throw Error(instruction.offset, info ? std::string(info->mnemonic) + ": " + std::string(message)
                                       : std::string(message));
}

template<class T>
const T& operand(const Instruction& instruction) {
  if (const auto* value = std::get_if<T>(&instruction.immediate))
    return *value;
  fail(instruction, "unexpected immediate type");
}

// The API accepts programmatically built modules, not just decoded binaries.
void check_immediate(const Instruction& instruction) {
  const auto* info = lookup(instruction.opcode);
  if (!info) {
    const auto [byte, extension] = instruction.opcode;
    constexpr std::string_view hex = "0123456789abcdef";
    std::string message = "unknown or unsupported Core 2.0 opcode 0x";
    message += hex[byte >> 4];
    message += hex[byte & 0x0f];
    if (extension != 0 || byte == 0xfc || byte == 0xfd)
      message += " with extension " + std::to_string(extension);
    throw Error(instruction.offset, message);
  }
  switch (info->format) {
  case Format::none: case Format::zero: case Format::zero_zero:
    operand<std::monostate>(instruction); break;
  case Format::block: operand<BlockType>(instruction); break;
  case Format::index: case Format::index_zero: case Format::i32: case Format::f32:
    operand<std::uint32_t>(instruction); break;
  case Format::index_pair: operand<IndexPair>(instruction); break;
  case Format::branch_table: operand<BranchTable>(instruction); break;
  case Format::types: operand<std::vector<Type>>(instruction); break;
  case Format::ref_type: operand<Type>(instruction); break;
  case Format::memory: operand<MemoryArg>(instruction); break;
  case Format::memory_lane: operand<MemoryLane>(instruction); break;
  case Format::lane: operand<std::uint8_t>(instruction); break;
  case Format::i64: case Format::f64: operand<std::uint64_t>(instruction); break;
  case Format::vector_bytes: operand<VectorBytes>(instruction); break;
  }
}

struct Context {
  explicit Context(const Module& module) : module(module) {}

  const Module& module;
  std::vector<const FunctionType*> functions;
  std::vector<TableType> tables;
  std::size_t memories{};
  std::vector<GlobalType> globals;
  std::size_t imported_globals{};
  std::unordered_set<std::uint32_t> declared_functions;

  const FunctionType& type(std::uint32_t index, std::size_t offset) const {
    return indexed(module.types, index, "type", offset);
  }

  const FunctionType& function(std::uint32_t index, std::size_t offset) const {
    return *indexed(functions, index, "function", offset);
  }

  const TableType& table(std::uint32_t index, std::size_t offset) const {
    return indexed(tables, index, "table", offset);
  }

  const GlobalType& global(std::uint32_t index, std::size_t offset) const {
    return indexed(globals, index, "global", offset);
  }

  void memory(std::uint32_t index, std::size_t offset) const {
    if (index >= memories)
      throw Error(offset, "memory index out of range: " + std::to_string(index));
  }

  const Element& element(std::uint32_t index, std::size_t offset) const {
    return indexed(module.elements, index, "element segment", offset);
  }

  void data(std::uint32_t index, const Instruction& instruction) const {
    if (!module.data_count)
      fail(instruction, "requires a data count section");
    indexed(module.data, index, "data segment", instruction.offset);
  }

  void ref_function(const Instruction& instruction) const {
    const auto index = operand<std::uint32_t>(instruction);
    function(index, instruction.offset);
    if (!declared_functions.contains(index))
      fail(instruction, "undeclared function reference");
  }
};

class Locals {
public:
  Locals(Types params, const FunctionBody& body) : params_(params) {
    std::uint64_t count = 0;
    for (const auto& run : body.locals) {
      value_type(run.type, body.offset);
      count += run.count;
      if (count > std::numeric_limits<std::uint32_t>::max())
        throw Error(body.offset, "too many local declarations");
      if (run.count != 0)
        runs_.push_back({static_cast<std::uint32_t>(count), run.type});
    }
  }

  Type at(std::uint32_t index, std::size_t offset) const {
    if (index < params_.size())
      return params_[index];
    // Search the exclusive run endpoints without expanding compressed locals.
    const auto run = std::ranges::upper_bound(runs_, index - params_.size(), {}, &Run::end);
    if (run == runs_.end())
      throw Error(offset, "local index out of range: " + std::to_string(index));
    return run->type;
  }

private:
  struct Run { std::uint32_t end; Type type; };
  Types params_;
  std::vector<Run> runs_;
};

class FunctionValidator {
public:
  FunctionValidator(const Context& context, const FunctionType& type, const FunctionBody& body)
      : context_(context), locals_(type.params, body) {
    enter(Kind::function, {}, type.results, body.offset);
    for (const auto& instruction : body.instructions) {
      instruction_ = &instruction;
      if (controls_.empty())
        error("unexpected instruction after function end");
      check_immediate(instruction);
      validate(instruction);
    }
    if (!controls_.empty())
      throw Error(controls_.back().offset, "missing end for function or control block");
  }

private:
  enum class Kind { function, block, loop, if_then, if_else };

  struct Frame {
    Kind kind;
    Types in, out;
    std::size_t height, offset;
    bool unreachable = false;
  };

  const Context& context_;
  Locals locals_;
  const Instruction* instruction_{};
  std::vector<StackType> stack_;
  std::vector<Frame> controls_;

  [[noreturn]] void error(std::string_view message) const {
    fail(*instruction_, message);
  }

  StackType pop(StackType expected = std::nullopt) {
    const auto& frame = controls_.back();
    if (stack_.size() == frame.height) {
      if (frame.unreachable)
        return std::nullopt; // Unknown, without popping across the frame boundary.
      error("operand stack underflow");
    }
    const auto actual = stack_.back();
    stack_.pop_back();
    if (actual && expected && actual != expected)
      error("operand type mismatch: expected " + std::string(type_name(*expected)) +
            ", got " + std::string(type_name(*actual)));
    return actual;
  }

  void pop_types(Types types) {
    for (auto it = types.rbegin(); it != types.rend(); ++it)
      pop(*it);
  }

  void push_types(Types types) {
    stack_.insert(stack_.end(), types.begin(), types.end());
  }

  void effect(std::initializer_list<Type> in, std::initializer_list<Type> out = {}) {
    pop_types({in.begin(), in.size()});
    push_types({out.begin(), out.size()});
  }

  void enter(Kind kind, Types in, Types out, std::size_t offset) {
    controls_.push_back({kind, in, out, stack_.size(), offset});
    push_types(in);
  }

  Frame leave() {
    const auto frame = controls_.back();
    pop_types(frame.out);
    if (stack_.size() != frame.height)
      error("unexpected operands at end of control arm");
    controls_.pop_back();
    return frame;
  }

  void unreachable() {
    auto& frame = controls_.back();
    stack_.resize(frame.height);
    frame.unreachable = true;
  }

  Types label(std::uint32_t depth) const {
    if (depth >= controls_.size())
      error("label depth out of range");
    const auto& frame = controls_[controls_.size() - depth - 1];
    return frame.kind == Kind::loop ? frame.in : frame.out;
  }

  void block(const BlockType& block_type, Kind kind, std::size_t offset) {
    Types in, out;
    if (const auto* single = std::get_if<Type>(&block_type.value)) {
      value_type(*single, offset);
      out = {single, 1};
    } else if (const auto* index = std::get_if<std::uint32_t>(&block_type.value)) {
      const auto& type = context_.type(*index, offset);
      in = type.params;
      out = type.results;
    } else if (!std::holds_alternative<std::monostate>(block_type.value)) {
      error("invalid block type");
    }
    if (kind == Kind::if_then)
      pop(i32);
    pop_types(in);
    enter(kind, in, out, offset);
  }

  void call(const FunctionType& type) {
    pop_types(type.params);
    push_types(type.results);
  }

  void memory_arg(const MemoryArg& arg, unsigned max_align) {
    context_.memory(0, instruction_->offset);
    if (arg.align > max_align)
      error("alignment exceeds the natural alignment");
  }

  void validate(const Instruction& instruction);
  bool scalar(unsigned op);
  bool misc(const Instruction& instruction);
  bool simd(const Instruction& instruction);
};

void FunctionValidator::validate(const Instruction& instruction) {
  const auto offset = instruction.offset;
  const auto index = [&] { return operand<std::uint32_t>(instruction); };
  switch (instruction.opcode.byte) {
  case 0x00: return unreachable();
  case 0x01: return;
  case 0x02: return block(operand<BlockType>(instruction), Kind::block, offset);
  case 0x03: return block(operand<BlockType>(instruction), Kind::loop, offset);
  case 0x04: return block(operand<BlockType>(instruction), Kind::if_then, offset);
  case 0x05: {
    if (controls_.back().kind != Kind::if_then)
      error("else without matching if");
    const auto frame = leave();
    return enter(Kind::if_else, frame.in, frame.out, frame.offset);
  }
  case 0x0b: {
    const auto frame = leave();
    if (frame.kind == Kind::if_then && !std::ranges::equal(frame.in, frame.out))
      error("if without else requires identical parameter and result types");
    if (!controls_.empty())
      push_types(frame.out);
    return;
  }
  case 0x0c:
    pop_types(label(index()));
    return unreachable();
  case 0x0d: {
    const auto target = label(index());
    pop(i32);
    pop_types(target);
    return push_types(target);
  }
  case 0x0e: {
    const auto& targets = operand<BranchTable>(instruction);
    const auto type = label(targets.otherwise);
    pop(i32);
    // All labels must have the same arity. On a polymorphic stack, an
    // unknown branch operand can satisfy different label types. Only the
    // suffix physically present above the frame's base needs comparison.
    const auto count = std::min(type.size(), stack_.size() - controls_.back().height);
    const auto args = std::span<const StackType>(stack_).last(count);
    for (const auto depth : targets.labels) {
      const auto other = label(depth);
      if (other.size() != type.size() || !std::ranges::equal(other.last(count), args,
              [](Type expected, StackType actual) { return !actual || expected == *actual; }))
        error("branch table label does not match the branch operands");
    }
    pop_types(type);
    return unreachable();
  }
  case 0x0f:
    pop_types(controls_.front().out);
    return unreachable();
  case 0x10:
    return call(context_.function(index(), offset));
  case 0x11: {
    const auto& indices = operand<IndexPair>(instruction);
    const auto& type = context_.type(indices.first, offset);
    if (context_.table(indices.second, offset).element != funcref)
      error("requires a funcref table");
    pop(i32);
    return call(type);
  }
  case 0x1a:
    pop();
    return;
  case 0x1b: {
    pop(i32);
    const auto second = pop();
    const auto first = pop(second);
    const auto type = second ? second : first;
    if (type && !is_numeric(*type))
      error("untyped select requires numeric or vector operands");
    stack_.push_back(type);
    return;
  }
  case 0x1c: {
    const auto& types = operand<std::vector<Type>>(instruction);
    if (types.size() != 1)
      error("typed select requires exactly one result type");
    const auto type = types.front();
    value_type(type, offset);
    return effect({type, type, i32}, {type});
  }
  case 0x20: return effect({}, {locals_.at(index(), offset)});
  case 0x21: return effect({locals_.at(index(), offset)});
  case 0x22: {
    const auto type = locals_.at(index(), offset);
    return effect({type}, {type});
  }
  case 0x23: return effect({}, {context_.global(index(), offset).type});
  case 0x24: {
    const auto& global = context_.global(index(), offset);
    if (!global.mutable_)
      error("cannot write an immutable global");
    return effect({global.type});
  }
  case 0x25: return effect({i32}, {context_.table(index(), offset).element});
  case 0x26: return effect({i32, context_.table(index(), offset).element});
  case 0x3f:
    context_.memory(0, offset);
    return effect({}, {i32});
  case 0x40:
    context_.memory(0, offset);
    return effect({i32}, {i32});
  case 0xd0: {
    const auto type = operand<Type>(instruction);
    if (!is_ref(type))
      error("invalid reference type");
    return effect({}, {type});
  }
  case 0xd1: {
    const auto type = pop();
    if (type && !is_ref(*type))
      error("expected a reference operand");
    return effect({}, {i32});
  }
  case 0xd2:
    context_.ref_function(instruction);
    return effect({}, {funcref});
  case 0xfc:
    if (misc(instruction))
      return;
    break;
  case 0xfd:
    if (simd(instruction))
      return;
    break;
  default:
    if (scalar(instruction.opcode.byte))
      return;
    break;
  }
  error("unsupported Core 2.0 validation rule");
}

bool FunctionValidator::scalar(unsigned op) {
  if (op >= 0x28 && op <= 0x3e) {
    struct Access { Type type; unsigned alignment; };
    static constexpr Access access[] = {
        {i32, 2}, {i64, 3}, {f32, 2}, {f64, 3},               // full loads
        {i32, 0}, {i32, 0}, {i32, 1}, {i32, 1},               // narrow i32 loads
        {i64, 0}, {i64, 0}, {i64, 1}, {i64, 1}, {i64, 2}, {i64, 2}, // narrow i64 loads
        {i32, 2}, {i64, 3}, {f32, 2}, {f64, 3},               // full stores
        {i32, 0}, {i32, 1}, {i64, 0}, {i64, 1}, {i64, 2}     // narrow stores
    };
    const auto [type, align] = access[op - 0x28];
    memory_arg(operand<MemoryArg>(*instruction_), align);
    if (op < 0x36)
      effect({i32}, {type});
    else
      effect({i32, type});
    return true;
  }

  if (op >= 0x41 && op <= 0x44) {
    static constexpr Type types[] = {i32, i64, f32, f64};
    effect({}, {types[op - 0x41]});
    return true;
  }

  struct Group { unsigned first, last, arity; Type in, out; };
  static constexpr Group groups[] = {
      {0x45, 0x45, 1, i32, i32}, {0x46, 0x4f, 2, i32, i32},
      {0x50, 0x50, 1, i64, i32}, {0x51, 0x5a, 2, i64, i32},
      {0x5b, 0x60, 2, f32, i32}, {0x61, 0x66, 2, f64, i32},
      {0x67, 0x69, 1, i32, i32}, {0x6a, 0x78, 2, i32, i32},
      {0x79, 0x7b, 1, i64, i64}, {0x7c, 0x8a, 2, i64, i64},
      {0x8b, 0x91, 1, f32, f32}, {0x92, 0x98, 2, f32, f32},
      {0x99, 0x9f, 1, f64, f64}, {0xa0, 0xa6, 2, f64, f64},
      {0xa7, 0xa7, 1, i64, i32}, {0xa8, 0xa9, 1, f32, i32},
      {0xaa, 0xab, 1, f64, i32}, {0xac, 0xad, 1, i32, i64},
      {0xae, 0xaf, 1, f32, i64}, {0xb0, 0xb1, 1, f64, i64},
      {0xb2, 0xb3, 1, i32, f32}, {0xb4, 0xb5, 1, i64, f32},
      {0xb6, 0xb6, 1, f64, f32}, {0xb7, 0xb8, 1, i32, f64},
      {0xb9, 0xba, 1, i64, f64}, {0xbb, 0xbb, 1, f32, f64},
      {0xbc, 0xbc, 1, f32, i32}, {0xbd, 0xbd, 1, f64, i64},
      {0xbe, 0xbe, 1, i32, f32}, {0xbf, 0xbf, 1, i64, f64},
      {0xc0, 0xc1, 1, i32, i32}, {0xc2, 0xc4, 1, i64, i64},
  };
  if (op >= 0x45 && op <= 0xc4) {
    const auto group = std::ranges::lower_bound(groups, op, {}, &Group::last);
    if (group != std::end(groups) && op >= group->first) {
      if (group->arity == 1)
        effect({group->in}, {group->out});
      else
        effect({group->in, group->in}, {group->out});
      return true;
    }
  }
  return false;
}

bool FunctionValidator::misc(const Instruction& instruction) {
  const auto op = instruction.opcode.extension;
  const auto offset = instruction.offset;
  const auto index = [&] { return operand<std::uint32_t>(instruction); };
  if (op <= 7) {
    effect({(op & 2) == 0 ? f32 : f64}, {op < 4 ? i32 : i64});
    return true;
  }
  switch (op) {
  case 8: // memory.init
    context_.data(index(), instruction);
    [[fallthrough]];
  case 10: // memory.copy
  case 11: // memory.fill
    context_.memory(0, offset);
    effect({i32, i32, i32});
    return true;
  case 9: // data.drop
    context_.data(index(), instruction);
    return true;
  case 12: { // table.init
    const auto [elem, table] = operand<IndexPair>(instruction);
    if (context_.element(elem, offset).type != context_.table(table, offset).element)
      error("element segment and table types do not match");
    effect({i32, i32, i32});
    return true;
  }
  case 13: // elem.drop
    context_.element(index(), offset);
    return true;
  case 14: { // table.copy
    const auto [dst, src] = operand<IndexPair>(instruction);
    if (context_.table(dst, offset).element != context_.table(src, offset).element)
      error("source and destination table types do not match");
    effect({i32, i32, i32});
    return true;
  }
  case 15: // table.grow
    effect({context_.table(index(), offset).element, i32}, {i32});
    return true;
  case 16: // table.size
    context_.table(index(), offset);
    effect({}, {i32});
    return true;
  case 17: // table.fill
    effect({i32, context_.table(index(), offset).element, i32});
    return true;
  default:
    return false;
  }
}

bool FunctionValidator::simd(const Instruction& instruction) {
  const auto op = instruction.opcode.extension;
  if (op <= 0x0b) {
    static constexpr unsigned align[] = {4, 3, 3, 3, 3, 3, 3, 0, 1, 2, 3, 4};
    memory_arg(operand<MemoryArg>(instruction), align[op]);
    if (op == 0x0b)
      effect({i32, v128});
    else
      effect({i32}, {v128});
    return true;
  }
  if (op == 0x0c) {
    effect({}, {v128});
    return true;
  }
  if (op == 0x0d)
    for (const auto lane : operand<VectorBytes>(instruction))
      if (lane >= 32)
        error("shuffle lane index out of range");
  if (op >= 0x0f && op <= 0x14) {
    static constexpr Type scalars[] = {i32, i32, i32, i64, f32, f64};
    effect({scalars[op - 0x0f]}, {v128});
    return true;
  }
  if (op >= 0x15 && op <= 0x22) {
    struct Lane { unsigned count; Type scalar; bool replace = false; };
    static constexpr Lane lanes[] = {
        {16, i32}, {16, i32}, {16, i32, true}, {8, i32}, {8, i32}, {8, i32, true},
        {4, i32}, {4, i32, true}, {2, i64}, {2, i64, true},
        {4, f32}, {4, f32, true}, {2, f64}, {2, f64, true}
    };
    const auto [count, scalar, replace] = lanes[op - 0x15];
    if (operand<std::uint8_t>(instruction) >= count)
      error("lane index out of range");
    if (replace)
      effect({v128, scalar}, {v128});
    else
      effect({v128}, {scalar});
    return true;
  }
  if (op >= 0x54 && op <= 0x5b) {
    const auto& lane = operand<MemoryLane>(instruction);
    const unsigned align = (op - 0x54) % 4;
    memory_arg(lane.memory, align);
    if (lane.lane >= (16u >> align))
      error("lane index out of range");
    if (op <= 0x57)
      effect({i32, v128}, {v128});
    else
      effect({i32, v128});
    return true;
  }
  if (op == 0x5c || op == 0x5d) {
    memory_arg(operand<MemoryArg>(instruction), op == 0x5c ? 2 : 3);
    effect({i32}, {v128});
    return true;
  }

  // Remaining instructions have no context-dependent operands or immediates.
  // Keep the groups explicit so adding an opcode to the decoder does not
  // implicitly admit it with a guessed signature.
  switch (op) {
  case 0x52: // bitselect
    effect({v128, v128, v128}, {v128});
    return true;
  case 0x53: case 0x63: case 0x64: case 0x83: case 0x84:
  case 0xa3: case 0xa4: case 0xc3: case 0xc4: // any_true, all_true, bitmask
    effect({v128}, {i32});
    return true;
  case 0x6b: case 0x6c: case 0x6d: case 0x8b: case 0x8c: case 0x8d:
  case 0xab: case 0xac: case 0xad: case 0xcb: case 0xcc: case 0xcd: // shifts
    effect({v128, i32}, {v128});
    return true;
  case 0x4d: case 0x5e: case 0x5f: case 0x60: case 0x61: case 0x62:
  case 0x67: case 0x68: case 0x69: case 0x6a: case 0x74: case 0x75: case 0x7a:
  case 0x7c: case 0x7d: case 0x7e: case 0x7f: case 0x80: case 0x81:
  case 0x87: case 0x88: case 0x89: case 0x8a: case 0x94:
  case 0xa0: case 0xa1: case 0xa7: case 0xa8: case 0xa9: case 0xaa:
  case 0xc0: case 0xc1: case 0xc7: case 0xc8: case 0xc9: case 0xca:
  case 0xe0: case 0xe1: case 0xe3: case 0xec: case 0xed: case 0xef:
  case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xff:
    effect({v128}, {v128});
    return true;
  default:
    break;
  }
  struct Range { unsigned first, last; };
  static constexpr Range binary[] = {
      {0x0d, 0x0e}, {0x23, 0x4c}, {0x4e, 0x51}, {0x65, 0x66},
      {0x6e, 0x73}, {0x76, 0x79}, {0x7b, 0x7b}, {0x82, 0x82},
      {0x85, 0x86}, {0x8e, 0x93}, {0x95, 0x99}, {0x9b, 0x9f},
      {0xae, 0xae}, {0xb1, 0xb1}, {0xb5, 0xba}, {0xbc, 0xbf},
      {0xce, 0xce}, {0xd1, 0xd1}, {0xd5, 0xdf}, {0xe4, 0xeb}, {0xf0, 0xf7}
  };
  const auto range = std::ranges::lower_bound(binary, op, {}, &Range::last);
  if (range != std::end(binary) && op >= range->first) {
    effect({v128, v128}, {v128});
    return true;
  }
  return false;
}

// Every Core 2.0 constant instruction produces one value with no inputs, so
// an initializer/offset expression must contain exactly one before its end.
void constant_expression(const Context& context, const Program& expression, Type expected) {
  const auto offset = expression.empty() ? 0 : expression.front().offset;
  if (expression.size() != 2 || expression.back().opcode != Opcode{0x0b})
    throw Error(offset, "expected one constant instruction and an expression end");
  const auto& instruction = expression.front();
  check_immediate(instruction);
  check_immediate(expression.back());

  Type actual{};
  switch (instruction.opcode.byte) {
  case 0x41: actual = i32; break;
  case 0x42: actual = i64; break;
  case 0x43: actual = f32; break;
  case 0x44: actual = f64; break;
  case 0xfd:
    if (instruction.opcode.extension != 0x0c)
      fail(instruction, "not allowed in a constant expression");
    actual = v128;
    break;
  case 0xd0:
    actual = operand<Type>(instruction);
    if (!is_ref(actual))
      fail(instruction, "invalid reference type");
    break;
  case 0xd2:
    context.ref_function(instruction);
    actual = funcref;
    break;
  case 0x23: {
    const auto index = operand<std::uint32_t>(instruction);
    const auto& global = context.global(index, offset);
    if (index >= context.imported_globals || global.mutable_)
      fail(instruction, "constant expressions may only read immutable imported globals");
    actual = global.type;
    break;
  }
  default:
    fail(instruction, "not allowed in a constant expression");
  }
  if (actual != expected)
    fail(instruction, "constant expression result type mismatch: expected " +
        std::string(type_name(expected)) + ", got " + std::string(type_name(actual)));
}

void declare_functions(Context& context, const Program& expression) {
  for (const auto& instruction : expression)
    if (instruction.opcode == Opcode{0xd2})
      context.declared_functions.insert(operand<std::uint32_t>(instruction));
}

void limits(const Limits& limits, std::string_view kind, std::uint32_t bound) {
  if (limits.min > bound || (limits.max && *limits.max > bound))
    throw Error(0, std::string(kind) + " limits exceed the Core 2.0 bound");
  if (limits.max && limits.min > *limits.max)
    throw Error(0, std::string(kind) + " minimum exceeds maximum");
}

void add_table(Context& context, const TableType& table) {
  if (!is_ref(table.element))
    throw Error(0, "table element type must be a reference type");
  limits(table.limits, "table", std::numeric_limits<std::uint32_t>::max());
  context.tables.push_back(table);
}

void add_memory(Context& context, const MemoryType& memory) {
  limits(memory.limits, "memory", 1u << 16);
  if (context.memories != 0)
    throw Error(0, "Core 2.0 allows at most one memory");
  ++context.memories;
}

void add_global(Context& context, const GlobalType& global) {
  value_type(global.type, 0);
  context.globals.push_back(global);
}

void validate_export(Context& context, const Export& entry) {
  switch (entry.kind) {
  case ExternalKind::function:
    context.function(entry.index, 0);
    context.declared_functions.insert(entry.index);
    return;
  case ExternalKind::table: context.table(entry.index, 0); return;
  case ExternalKind::memory: context.memory(entry.index, 0); return;
  case ExternalKind::global: context.global(entry.index, 0); return;
  }
  throw Error(0, "unknown export kind");
}

} // namespace

void validate_module(const Module& module) {
  Context context(module);
  for (const auto& type : module.types) {
    for (const auto param : type.params)
      value_type(param, 0);
    for (const auto result : type.results)
      value_type(result, 0);
  }
  for (const auto& import : module.imports) {
    if (const auto* index = std::get_if<std::uint32_t>(&import.desc))
      context.functions.push_back(&context.type(*index, 0));
    else if (const auto* table = std::get_if<TableType>(&import.desc))
      add_table(context, *table);
    else if (const auto* memory = std::get_if<MemoryType>(&import.desc))
      add_memory(context, *memory);
    else if (const auto* global = std::get_if<GlobalType>(&import.desc))
      add_global(context, *global);
    else
      throw Error(0, "invalid import descriptor");
  }
  context.imported_globals = context.globals.size();
  for (const auto& function : module.functions)
    context.functions.push_back(&context.type(function.type, function.body.offset));
  for (const auto& table : module.tables)
    add_table(context, table);
  for (const auto& memory : module.memories)
    add_memory(context, memory);
  for (const auto& global : module.globals)
    add_global(context, global.type);

  if (module.start) {
    const auto& start = context.function(*module.start, 0);
    if (!start.params.empty() || !start.results.empty())
      throw Error(0, "start function must have no parameters or results");
  }
  if (module.data_count && *module.data_count != module.data.size())
    throw Error(0, "data count does not match the number of data segments");
  std::unordered_set<std::string_view> exports;
  for (const auto& entry : module.exports) {
    if (!exports.insert(entry.name).second)
      throw Error(0, "duplicate export name: " + entry.name);
    validate_export(context, entry);
  }

  // ref.func in a body needs a declaration outside of the function bodies:
  // a function export, a global initializer, or an element segment.
  for (const auto& global : module.globals)
    declare_functions(context, global.init);
  for (const auto& ele : module.elements)
    for (const auto& init : ele.init)
      declare_functions(context, init);

  for (const auto& global : module.globals)
    constant_expression(context, global.init, global.type.type);
  for (const auto& ele : module.elements) {
    if (!is_ref(ele.type))
      throw Error(0, "element segment type must be a reference type");
    switch (ele.mode) {
    case SegmentMode::active:
      if (context.table(ele.table, 0).element != ele.type)
        throw Error(0, "element segment and table types do not match");
      constant_expression(context, ele.offset, i32);
      break;
    case SegmentMode::passive: case SegmentMode::declarative:
      if (ele.table != 0 || !ele.offset.empty())
        throw Error(0, "inactive element segment carries an active table or offset");
      break;
    default:
      throw Error(0, "invalid element segment mode");
    }
    for (const auto& init : ele.init)
      constant_expression(context, init, ele.type);
  }
  for (const auto& data : module.data) {
    switch (data.mode) {
    case SegmentMode::active:
      context.memory(data.memory, 0);
      constant_expression(context, data.offset, i32);
      break;
    case SegmentMode::passive:
      if (data.memory != 0 || !data.offset.empty())
        throw Error(0, "passive data segment carries an active memory or offset");
      break;
    default:
      throw Error(0, "invalid data segment mode");
    }
  }
  for (const auto& function : module.functions)
    FunctionValidator(context, context.type(function.type, function.body.offset), function.body);
}

} // namespace limestone::wasmutils::core
