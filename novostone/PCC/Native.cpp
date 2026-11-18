#include "novostone/PCC/Native.hpp"
#include "novostone/PCC/SMTProof.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace novostone::pcc {
namespace {

using R = NativeRegister;
using O = NativeOp;
using I = NativeInstruction;
using H = NativeHome;

constexpr std::array arguments{R::Rdi, R::Rsi, R::Rdx, R::Rcx, R::R8, R::R9};
constexpr std::array volatile_regs{R::Rax, R::Rcx, R::Rdx, R::Rsi, R::Rdi,
                                   R::R8, R::R9, R::R10, R::R11};
constexpr std::array caller_homes{R::R8, R::R9, R::Rcx, R::Rdx, R::Rsi, R::Rdi};
constexpr std::array callee_homes{R::Rbx, R::R12, R::R13, R::R14, R::R15, R::Rbp};
constexpr std::size_t register_count = 15;
constexpr std::uint32_t slot_size = 8;
constexpr std::uint32_t frame_limit = 32 * 1024;
constexpr std::size_t ir_limit = 4096;
constexpr std::size_t native_limit = 32768;
constexpr std::size_t script_limit = 2 * 1024 * 1024;

struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };

void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

void within(bool condition, const char* message) {
  if (!condition) throw Unsupported(message);
}

template <class Range>
bool contains(const Range& range, R reg) {
  return std::find(range.begin(), range.end(), reg) != range.end();
}

std::size_t index(R reg) {
  const auto n = static_cast<std::size_t>(reg);
  require(n < register_count, "invalid x86-64 register");
  return n;
}

bool symbol_ok(std::string_view symbol) {
  const auto start = [](char c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };
  return !symbol.empty() && symbol.size() <= 128 && start(symbol.front()) &&
         std::all_of(symbol.begin(), symbol.end(), [&](char c) {
           return start(c) || (c >= '0' && c <= '9');
         });
}

using IRCode = std::vector<ir::Instruction>;

const IRCode& subject(const ir::Function& f, const NativeRequest& request,
                      const NativeLimits& limits) {
  require(symbol_ok(request.symbol), "invalid native assembly symbol");
  within(f.blocks.size() <= 1, "native lowering requires one straight-line IR block");
  const auto it = f.blocks.find(f.entry);
  require(it != f.blocks.end(), "missing IR entry block");
  const auto& code = it->second.instructions;
  const auto cap = std::min(ir_limit, limits.max_ir_instructions);
  within(code.size() <= cap && request.identity_calls.size() <= cap,
         "native IR instruction budget exhausted");
  for (const auto& i : code) {
    // Bounds before passing untrusted vectors to the ordinary IR verifier.
    require(i.operands.size() <= 3 && i.targets.size() <= 2,
            "malformed straight-line IR operand or target list");
  }
  ir::verify(f);
  within(f.arity <= arguments.size(), "SysV stack arguments are not modeled");
  std::size_t call_count = 0;
  for (const auto& i : code) {
    switch (i.op) {
      case ir::Op::Param: case ir::Op::Const: case ir::Op::Copy:
      case ir::Op::Add: case ir::Op::Sub: case ir::Op::Mul:
      case ir::Op::Eq: case ir::Op::Ult: case ir::Op::Select:
      case ir::Op::Return:
        break;
      default:
        throw Unsupported("IR opcode not supported by native lowering");
    }
    if (request.identity_calls.count(i.id)) {
      require(i.op == ir::Op::Copy, "identity helper requested for an instruction other than Copy");
      ++call_count;
    }
  }
  require(call_count == request.identity_calls.size(), "unknown identity call site");
  return code;
}

I ins(O op, R dst = R::Rax, R src = R::Rax, std::uint32_t integer = 0) {
  return {op, dst, src, integer};
}

void read_home(std::vector<I>& code, R dst, const H& home) {
  if (home.kind == H::Kind::Spill)
    code.push_back(ins(O::Load32, dst, R::Rax, home.offset));
  else if (dst != home.reg)
    code.push_back(ins(O::Mov32, dst, home.reg));
}

void write_home(std::vector<I>& code, const H& home, R src) {
  if (home.kind == H::Kind::Spill)
    code.push_back(ins(O::Store32, R::Rax, src, home.offset));
  else if (src != home.reg)
    code.push_back(ins(O::Mov32, home.reg, src));
}

void check_native_size(const NativeProgram& p, std::size_t values, NativeLimits limits) {
  require(p.homes.size() <= values && p.body.size() <= values &&
              p.frame.arguments.size() <= arguments.size() &&
              p.frame.saved.size() <= callee_homes.size(),
          "unexpected native metadata");
  auto remaining = std::min(limits.max_native_instructions, native_limit);
  const auto use = [&](std::size_t size) {
    within(size <= remaining, "native instruction budget exhausted");
    remaining -= size;
  };
  use(p.prologue.size());
  use(p.epilogue.size());
  use(p.identity_helper.size());
  for (const auto& span : p.body) use(span.code.size());
}

NativeProgram generate(const IRCode& code, const NativeRequest& request) {
  std::map<ir::Id, std::size_t> end;
  std::set<std::size_t> calls;
  std::set<std::uint32_t> arg_slots;
  for (std::size_t n = 0; n < code.size(); ++n) {
    const auto& i = code[n];
    if (ir::produces_value(i.op)) end.emplace(i.id, n);
    for (auto operand : i.operands) end[operand] = n;
    if (request.identity_calls.count(i.id)) calls.insert(n);
    if (i.op == ir::Op::Param) arg_slots.insert(i.immediate);
  }

  NativeProgram p;
  std::map<R, std::size_t> occupied;
  std::set<R> saved;
  for (std::size_t n = 0; n < code.size(); ++n) {
    const auto& i = code[n];
    if (!ir::produces_value(i.op)) continue;
    const auto call = calls.upper_bound(n);
    const bool crosses_call = call != calls.end() && *call < end.at(i.id);
    const auto free_reg = [&](const auto& pool) -> std::optional<R> {
      for (auto r : pool)
        if (!occupied.count(r) || occupied.at(r) <= n) return r;
      return std::nullopt;
    };
    auto reg = crosses_call ? std::nullopt : free_reg(caller_homes);
    if (!reg) reg = free_reg(callee_homes);
    if (reg) {
      occupied[*reg] = end.at(i.id);
      if (contains(callee_homes, *reg)) saved.insert(*reg);
      p.homes.emplace(i.id, H{H::Kind::Register, *reg, 0});
    } else {
      p.homes.emplace(i.id, H{H::Kind::Spill, R::Rax, 0});
    }
  }

  std::uint32_t used = 0;
  const auto next_slot = [&] {
    const auto offset = used;
    used += slot_size;
    return offset;
  };
  for (auto a : arg_slots) p.frame.arguments.emplace(a, next_slot());
  for (auto r : saved) p.frame.saved.emplace(r, next_slot());
  for (auto& [id, home] : p.homes) {
    (void)id;
    if (home.kind == H::Kind::Spill) home.offset = next_slot();
  }
  if (used || !calls.empty()) p.frame.size = (used + 7u) / 16u * 16u + 8u;
  within(p.frame.size <= frame_limit, "native stack frame exceeds the target limit");

  if (p.frame.size)
    p.prologue.push_back(ins(O::SubSp, R::Rax, R::Rax, p.frame.size));
  for (const auto& [reg, offset] : p.frame.saved)
    p.prologue.push_back(ins(O::Save64, R::Rax, reg, offset));
  for (const auto& [arg, offset] : p.frame.arguments)
    p.prologue.push_back(ins(O::Store32, R::Rax, arguments.at(arg), offset));

  for (const auto& i : code) {
    std::vector<I> out;
    const auto use = [&](R dst, std::size_t operand) {
      read_home(out, dst, p.homes.at(i.operands.at(operand)));
    };
    switch (i.op) {
      case ir::Op::Param:
        out.push_back(ins(O::Load32, R::Rax, R::Rax, p.frame.arguments.at(i.immediate)));
        break;
      case ir::Op::Const:
        out.push_back(ins(O::Imm32, R::Rax, R::Rax, i.immediate));
        break;
      case ir::Op::Copy:
        if (request.identity_calls.count(i.id)) {
          use(R::Rdi, 0);
          out.push_back(ins(O::CallIdentity));
        } else use(R::Rax, 0);
        break;
      case ir::Op::Add: case ir::Op::Sub: case ir::Op::Mul:
        use(R::Rax, 0);
        use(R::R10, 1);
        out.push_back(ins(i.op == ir::Op::Add ? O::Add32 :
                          i.op == ir::Op::Sub ? O::Sub32 : O::Mul32, R::Rax, R::R10));
        break;
      case ir::Op::Eq: case ir::Op::Ult:
        use(R::R10, 0);
        use(R::R11, 1);
        out.push_back(ins(O::Imm32));
        out.push_back(ins(O::Compare32, R::R10, R::R11));
        out.push_back(ins(i.op == ir::Op::Eq ? O::SetEqual8 : O::SetBelow8));
        break;
      case ir::Op::Select:
        use(R::R10, 0);
        use(R::R11, 1);
        use(R::Rax, 2);
        out.push_back(ins(O::CompareZero32, R::R10));
        out.push_back(ins(O::MoveNonzero32, R::Rax, R::R11));
        break;
      case ir::Op::Return:
        use(R::Rax, 0);
        break;
      default:
        throw std::logic_error("unsupported opcode passed native preflight");
    }
    if (ir::produces_value(i.op)) write_home(out, p.homes.at(i.id), R::Rax);
    p.body.push_back({i.id, std::move(out)});
  }

  for (auto it = p.frame.saved.rbegin(); it != p.frame.saved.rend(); ++it)
    p.epilogue.push_back(ins(O::Restore64, it->first, R::Rax, it->second));
  if (p.frame.size)
    p.epilogue.push_back(ins(O::AddSp, R::Rax, R::Rax, p.frame.size));
  p.epilogue.push_back(ins(O::Ret));

  if (!calls.empty()) {
    p.identity_helper.push_back(ins(O::Mov32, R::Rax, R::Rdi));
    // Actually destroy the caller's other volatile GPRs. The verifier's
    // summary is less precise and treats them as independently arbitrary.
    for (auto r : volatile_regs)
      if (r != R::Rax) p.identity_helper.push_back(ins(O::Imm32, r, R::Rax, 0xc10bbe00u));
    p.identity_helper.push_back(ins(O::Ret));
  }
  return p;
}

void instruction_shape(const I& i) {
  (void)index(i.dst);
  (void)index(i.src);
  bool dst = false, src = false, integer = false;
  switch (i.op) {
    case O::SubSp: case O::AddSp: integer = true; break;
    case O::Imm32: case O::Load32: case O::Restore64: dst = integer = true; break;
    case O::Store32: case O::Save64: src = integer = true; break;
    case O::Mov32: case O::Add32: case O::Sub32: case O::Mul32: case O::Compare32:
    case O::MoveNonzero32:
      dst = src = true; break;
    case O::CompareZero32: case O::SetEqual8: case O::SetBelow8: dst = true; break;
    case O::CallIdentity: case O::Ret: break;
    default: throw std::invalid_argument("unknown machine opcode");
  }
  require((dst || i.dst == R::Rax) && (src || i.src == R::Rax) &&
              (integer || i.integer == 0),
          "noncanonical unused machine operand");
}

struct SlotKinds {
  std::map<std::uint32_t, std::uint32_t> args;
  std::map<std::uint32_t, R> saves;
  std::set<std::uint32_t> spills;
};

SlotKinds check_layout(const IRCode& code, const NativeProgram& p) {
  require(p.frame.size <= frame_limit && p.frame.size % slot_size == 0,
          "invalid stack frame size");
  SlotKinds slots;
  std::set<std::uint32_t> occupied;
  const auto claim = [&](std::uint32_t offset) {
    require(offset % slot_size == 0 && p.frame.size >= slot_size &&
                offset <= p.frame.size - slot_size,
            "unaligned or out-of-frame stack slot");
    require(occupied.insert(offset).second, "overlapping frame slots");
  };
  std::set<std::uint32_t> required_args;
  std::set<R> required_saves;
  std::size_t definitions = 0;
  for (const auto& i : code) {
    if (i.op == ir::Op::Param) required_args.insert(i.immediate);
    if (!ir::produces_value(i.op)) continue;
    ++definitions;
    const auto it = p.homes.find(i.id);
    require(it != p.homes.end(), "missing native value home");
    const auto& home = it->second;
    if (home.kind == H::Kind::Spill) {
      require(home.reg == R::Rax, "noncanonical spill home");
      claim(home.offset);
      slots.spills.insert(home.offset);
    } else {
      require(home.kind == H::Kind::Register && home.offset == 0 &&
                  (contains(caller_homes, home.reg) || contains(callee_homes, home.reg)),
              "invalid or reserved native register home");
      if (contains(callee_homes, home.reg)) required_saves.insert(home.reg);
    }
  }
  require(p.homes.size() == definitions && p.frame.arguments.size() == required_args.size(),
          "unexpected native value homes or argument slots");
  for (const auto& [a, offset] : p.frame.arguments) {
    require(required_args.count(a) != 0, "unknown incoming argument slot");
    claim(offset);
    slots.args.emplace(offset, a);
  }
  for (const auto& [r, offset] : p.frame.saved) {
    require(contains(callee_homes, r), "save slot for a non-callee-saved register");
    required_saves.erase(r);
    claim(offset);
    slots.saves.emplace(offset, r);
  }
  require(required_saves.empty(), "missing callee-saved register slot");
  return slots;
}

void check_liveness(const IRCode& code, const NativeProgram& p, const NativeRequest& request) {
  // Compute liveness backwards independently of the producer's interval
  // scan. At each boundary, a register can belong to at most one live value.
  std::set<ir::Id> live;
  for (auto i = code.rbegin(); i != code.rend(); ++i) {
    std::map<R, ir::Id> regs;
    for (auto id : live) {
      const auto& home = p.homes.at(id);
      if (home.kind != H::Kind::Register) continue;
      require(regs.emplace(home.reg, id).second, "interfering native register homes");
      if (id != i->id && request.identity_calls.count(i->id))
        require(!contains(volatile_regs, home.reg), "live-through value in call-clobbered register");
    }
    if (ir::produces_value(i->op)) {
      const auto& home = p.homes.at(i->id);
      if (home.kind == H::Kind::Register) {
        const auto use = regs.find(home.reg);
        require(use == regs.end() || use->second == i->id, "native definition overwrites a live home");
      }
    }
    live.erase(i->id);
    live.insert(i->operands.begin(), i->operands.end());
  }
  require(live.empty(), "value live before function entry");
}

void check_boundary_code(const NativeProgram& p, const SlotKinds& slots) {
  const auto adjust = [&](O op) { return ins(op, R::Rax, R::Rax, p.frame.size); };
  auto args = slots.args;
  auto saves = slots.saves;
  std::size_t n = 0;
  if (p.frame.size) {
    require(!p.prologue.empty() && p.prologue.front() == adjust(O::SubSp),
            "missing or wrong prologue stack adjustment");
    ++n;
  }
  for (; n < p.prologue.size(); ++n) {
    const auto& i = p.prologue[n];
    instruction_shape(i);
    if (i.op == O::Save64) {
      const auto it = saves.find(i.integer);
      require(it != saves.end() && it->second == i.src, "wrong prologue callee save");
      saves.erase(it);
    } else {
      require(i.op == O::Store32, "unexpected prologue instruction");
      const auto it = args.find(i.integer);
      require(it != args.end() && arguments.at(it->second) == i.src,
              "wrong SysV argument capture");
      args.erase(it);
    }
  }
  require(args.empty() && saves.empty(), "missing argument capture or callee save");

  require(!p.epilogue.empty() && p.epilogue.back() == ins(O::Ret), "missing native return");
  auto end = p.epilogue.size() - 1;
  if (p.frame.size) {
    require(end > 0 && p.epilogue[end - 1] == adjust(O::AddSp),
            "missing or wrong epilogue stack adjustment");
    --end;
  }
  saves = slots.saves;
  for (n = 0; n < end; ++n) {
    const auto& i = p.epilogue[n];
    instruction_shape(i);
    require(i.op == O::Restore64, "unexpected epilogue instruction");
    const auto it = saves.find(i.integer);
    require(it != saves.end() && it->second == i.dst, "wrong epilogue callee restore");
    saves.erase(it);
  }
  require(saves.empty(), "missing callee restore");
}

void check_helper(const NativeProgram& p, const NativeRequest& request) {
  if (request.identity_calls.empty()) {
    require(p.identity_helper.empty(), "unexpected identity helper");
    return;
  }
  require(p.frame.size % 16 == 8, "SysV call-site RSP is not 16-byte aligned");
  require(!p.identity_helper.empty() && p.identity_helper.back() == ins(O::Ret),
          "identity helper does not return normally");
  // Track whether each low word is *the* incoming arg, independent of
  // all other caller state. No stack accesses, calls, or callee writes.
  std::array<bool, register_count> arg{};
  arg[index(R::Rdi)] = true;
  for (auto it = p.identity_helper.begin(); it != std::prev(p.identity_helper.end()); ++it) {
    instruction_shape(*it);
    require((it->op == O::Mov32 || it->op == O::Imm32) &&
                contains(volatile_regs, it->dst),
            "instruction outside the local identity helper contract");
    arg[index(it->dst)] = it->op == O::Mov32 && arg[index(it->src)];
  }
  require(arg[index(R::Rax)], "local helper does not return its incoming i32 argument");
}

// Symbolic i32 terms. A value ID becomes an independent variable only after
// adding the obligation establishing the value in its home. This strengthens
// later obligations: they may not rely on accidental relations between SSA
// values or on a particular volatile register value after a call.
using Term = std::size_t;
enum class Expr { Input, Constant, Add, Sub, Mul, Eq, Ult, Select, Set8 };

struct Terms {
  using Node = std::tuple<Expr, Term, Term, Term>;
  std::map<Node, Term> ids;
  std::vector<Node> nodes;
  std::vector<std::size_t> depths;
  std::vector<std::pair<Term, Term>> equalities;
  Term fresh = 0;

  Term make(Expr op, Term a = 0, Term b = 0, Term c = 0) {
    if ((op == Expr::Add || op == Expr::Mul || op == Expr::Eq) && a > b)
      std::swap(a, b);
    if (op == Expr::Set8 && nodes.at(a) == Node{Expr::Constant, 0, 0, 0})
      return b;
    if (op == Expr::Select) {
      const auto& [test, l, r, _] = nodes.at(a);
      const auto zero = Node{Expr::Constant, 0, 0, 0};
      // CMOVNE following TEST x,x is the IR's nonzero test.
      if (test == Expr::Eq) {
        if (nodes.at(l) == zero) return make(Expr::Select, r, c, b);
        if (nodes.at(r) == zero) return make(Expr::Select, l, c, b);
      }
    }
    const Node key{op, a, b, c};
    const auto [at, added] = ids.emplace(key, nodes.size());
    if (added) {
      std::size_t depth = 1;
      if (op != Expr::Input && op != Expr::Constant) {
        depth += std::max(depths.at(a), depths.at(b));
        if (op == Expr::Select) depth = std::max(depth, 1 + depths.at(c));
      }
      within(depth <= 128, "native symbolic expression depth exhausted");
      nodes.push_back(key);
      depths.push_back(depth);
    }
    return at->second;
  }

  Term input() { return make(Expr::Input, ++fresh); }
  Term constant(std::uint32_t v) { return make(Expr::Constant, v); }
};

void bind_body(const IRCode& code, const NativeProgram& p,
               const NativeRequest& request, const SlotKinds& slots, Terms& t) {
  require(p.body.size() == code.size(), "native spans do not cover the IR");
  std::array<Term, register_count> regs{};
  for (auto& reg : regs) reg = t.input();
  // Prologue captures the low words of the actual SysV input registers
  // without modifying any GPR. Boundary instructions were checked above.
  const auto incoming = regs;
  std::map<std::uint32_t, Term> stack;
  for (const auto& [offset, arg] : slots.args)
    stack.emplace(offset, incoming[index(arguments.at(arg))]);
  for (auto offset : slots.spills) stack.emplace(offset, t.input());
  std::map<ir::Id, Term> values;
  std::optional<Term> zf, cf;
  const auto home = [&](ir::Id id) -> Term& {
    const auto& h = p.homes.at(id);
    return h.kind == H::Kind::Spill ? stack.at(h.offset) : regs.at(index(h.reg));
  };
  const auto writable = [&](R dst) {
    require(contains(volatile_regs, dst) || p.frame.saved.count(dst) != 0,
            "machine body writes an unsaved callee-saved register");
  };
  for (std::size_t n = 0; n < code.size(); ++n) {
    const auto& ir = code[n];
    const auto& span = p.body[n];
    require(span.instruction == ir.id, "native span does not bind to this IR instruction");
    for (auto operand : ir.operands)
      require(home(operand) == values.at(operand), "SSA operand is not available in its native home");
    const auto src = [&](std::size_t operand) { return values.at(ir.operands.at(operand)); };
    const auto bin = [&](Expr op) { return t.make(op, src(0), src(1)); };
    const bool needs_call = request.identity_calls.count(ir.id) != 0;
    bool called = false;
    for (const auto& i : span.code) {
      instruction_shape(i);
      auto& dst = regs[index(i.dst)];
      const auto operand = regs[index(i.src)];
      switch (i.op) {
        case O::Mov32: writable(i.dst); dst = operand; break;
        case O::Imm32: writable(i.dst); dst = t.constant(i.integer); break;
        case O::Load32:
          writable(i.dst);
          require(stack.count(i.integer) != 0, "load outside the value slots");
          dst = stack.at(i.integer);
          break;
        case O::Store32:
          require(slots.spills.count(i.integer) != 0, "store outside the spill slots");
          stack.at(i.integer) = operand;
          break;
        case O::Add32: case O::Sub32: case O::Mul32:
          writable(i.dst);
          dst = t.make(i.op == O::Add32 ? Expr::Add :
                       i.op == O::Sub32 ? Expr::Sub : Expr::Mul, dst, operand);
          zf.reset(); cf.reset(); // conservative: no arithmetic flag reuse
          break;
        case O::Compare32:
          zf = t.make(Expr::Eq, dst, operand);
          cf = t.make(Expr::Ult, dst, operand);
          break;
        case O::CompareZero32:
          zf = t.make(Expr::Eq, dst, t.constant(0));
          cf = t.constant(0);
          break;
        case O::SetEqual8: case O::SetBelow8: {
          writable(i.dst);
          const auto flag = i.op == O::SetEqual8 ? zf : cf;
          require(flag.has_value(), "SETcc reads unmodeled flags");
          dst = t.make(Expr::Set8, dst, *flag);
          break;
        }
        case O::MoveNonzero32:
          writable(i.dst);
          require(zf.has_value(), "CMOVNE reads unmodeled flags");
          dst = t.make(Expr::Select, *zf, dst, operand);
          break;
        case O::CallIdentity: {
          require(needs_call && !called, "unexpected modeled call site");
          const auto arg = regs[index(R::Rdi)];
          // The call itself must implement this Copy, not merely appear in
          // the span while unrelated code computes the result.
          require(arg == src(0), "wrong SysV argument at modeled call");
          for (auto reg : volatile_regs) regs[index(reg)] = t.input();
          regs[index(R::Rax)] = arg;
          zf.reset(); cf.reset();
          called = true;
          break;
        }
        default: throw std::invalid_argument("stack adjustment, return or save/restore inside native body");
      }
    }
    require(called == needs_call, "missing requested modeled call");
    Term expected = 0;
    switch (ir.op) {
      case ir::Op::Param: expected = incoming[index(arguments.at(ir.immediate))]; break;
      case ir::Op::Const: expected = t.constant(ir.immediate); break;
      case ir::Op::Copy: case ir::Op::Return: expected = src(0); break;
      case ir::Op::Add: expected = bin(Expr::Add); break;
      case ir::Op::Sub: expected = bin(Expr::Sub); break;
      case ir::Op::Mul: expected = bin(Expr::Mul); break;
      case ir::Op::Eq: expected = bin(Expr::Eq); break;
      case ir::Op::Ult: expected = bin(Expr::Ult); break;
      case ir::Op::Select: expected = t.make(Expr::Select, src(0), src(1), src(2)); break;
      default: throw std::logic_error("unsupported IR instruction reached native checker");
    }
    if (ir.op == ir::Op::Return) {
      // Abstract epilogue cannot write RAX; check its pre-epilogue low word.
      t.equalities.emplace_back(regs[index(R::Rax)], expected);
    } else {
      auto& result = home(ir.id);
      t.equalities.emplace_back(result, expected);
      result = t.input();
      values.emplace(ir.id, result);
    }
  }
}

// Acyclic, definitional Boolean circuit in SMT-LIB2. The only nondeterminism
// is the unconstrained input bits; the final assertion asks for a mismatch.
// Bit-level Add/Mul encode wraparound rather than C++ signed arithmetic.
class Circuit {
  using Bit = std::size_t;
  using Word = std::array<Bit, 32>;
  enum class Gate { And, Or, Xor, Not, Ite };

  const Terms& terms_;
  std::size_t budget_;
  std::string script_ = "; Novostone PCC x86-64 i32 local translation check v1\n(set-logic QF_UF)\n";
  Bit next_ = 2;
  std::vector<std::optional<Word>> words_;
  std::map<std::tuple<Gate, Bit, Bit, Bit>, Bit> gates_;

  void append(const std::string& text) {
    within(script_.size() <= budget_ && text.size() <= budget_ - script_.size(),
           "native SMT byte budget exhausted");
    script_ += text;
  }

  static std::string name(Bit bit) {
    if (bit == 0) return "false";
    if (bit == 1) return "true";
    return "b" + std::to_string(bit);
  }

  Bit variable() {
    const auto bit = next_++;
    append("(declare-const " + name(bit) + " Bool)\n");
    return bit;
  }

  Bit gate(Gate op, Bit a, Bit b = 0, Bit c = 0) {
    if (op == Gate::And || op == Gate::Or || op == Gate::Xor)
      if (a > b) std::swap(a, b);
    if (op == Gate::And) {
      if (!a || a == b) return a;
      if (a == 1) return b;
    } else if (op == Gate::Or) {
      if (!a || a == b) return b;
      if (a == 1) return a;
    } else if (op == Gate::Xor) {
      if (a == b) return 0;
      if (!a) return b;
      if (a == 1) return gate(Gate::Not, b);
    } else if (op == Gate::Not) {
      if (a < 2) return 1 - a;
    } else {
      if (a == 0 || b == c) return c;
      if (a == 1) return b;
    }
    const auto key = std::make_tuple(op, a, b, c);
    if (const auto it = gates_.find(key); it != gates_.end()) return it->second;
    const auto bit = variable();
    const auto x = name(a), y = name(b), z = name(c);
    // Satie's SMT-LIB2 Boolean frontend supports these core connectives,
    // but not its `xor` and `ite` spellings.
    const auto expr =
        op == Gate::And ? "(and " + x + " " + y + ")" :
        op == Gate::Or  ? "(or " + x + " " + y + ")" :
        op == Gate::Xor ? "(not (= " + x + " " + y + "))" :
        op == Gate::Not ? "(not " + x + ")" :
        "(or (and " + x + " " + y + ") (and (not " + x + ") " + z + "))";
    append("(assert (= " + name(bit) + " " + expr + "))\n");
    gates_.emplace(key, bit);
    return bit;
  }

  Word sum(const Word& a, const Word& b, bool subtract = false) {
    Word out{};
    Bit carry = subtract;
    for (std::size_t i = 0; i < out.size(); ++i) {
      const auto bi = subtract ? gate(Gate::Not, b[i]) : b[i];
      const auto x = gate(Gate::Xor, a[i], bi);
      out[i] = gate(Gate::Xor, x, carry);
      if (i + 1 != out.size())
        carry = gate(Gate::Or, gate(Gate::And, a[i], bi), gate(Gate::And, x, carry));
    }
    return out;
  }

  Word multiply(const Word& a, const Word& b) {
    Word out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
      Word row{};
      for (std::size_t j = 0; j + i < out.size(); ++j)
        row[j + i] = gate(Gate::And, a[i], b[j]);
      out = sum(out, row);
    }
    return out;
  }

  Bit equal(const Word& a, const Word& b) {
    Bit different = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
      different = gate(Gate::Or, different, gate(Gate::Xor, a[i], b[i]));
    return gate(Gate::Not, different);
  }

  Bit below(const Word& a, const Word& b) {
    Bit less = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
      less = gate(Gate::Ite, gate(Gate::Xor, a[i], b[i]), b[i], less);
    return less;
  }

  const Word& word(Term id) {
    if (words_.at(id)) return *words_.at(id);
    const auto [op, a, b, c] = terms_.nodes.at(id);
    Word out{};
    switch (op) {
      case Expr::Input:
        for (auto& bit : out) bit = variable();
        break;
      case Expr::Constant:
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = (a >> i) & 1u;
        break;
      case Expr::Add: case Expr::Sub: out = sum(word(a), word(b), op == Expr::Sub); break;
      case Expr::Mul: out = multiply(word(a), word(b)); break;
      case Expr::Eq: out[0] = equal(word(a), word(b)); break;
      case Expr::Ult: out[0] = below(word(a), word(b)); break;
      case Expr::Select: {
        const auto check = equal(word(a), Word{});
        const auto& yes = word(b);
        const auto& no = word(c);
        for (std::size_t i = 0; i < out.size(); ++i)
          out[i] = gate(Gate::Ite, check, no[i], yes[i]);
        break;
      }
      case Expr::Set8:
        out = word(a);
        std::fill_n(out.begin(), 8, Bit{0});
        out[0] = word(b)[0];
        break;
    }
    return words_.at(id).emplace(out);
  }

public:
  Circuit(const Terms& t, std::size_t budget)
      : terms_(t), budget_(budget), words_(t.nodes.size()) {}

  std::string encode() {
    // Identical machine/reference terms have an exact reflexivity proof
    // without bit-blasting (especially valuable for common IMUL spans).
    // Nonidentical terms are checked on all 32 bits.
    Bit mismatch = 0;
    for (const auto& [actual, expected] : terms_.equalities) {
      if (actual == expected) continue;
      mismatch = gate(Gate::Or, mismatch,
                      gate(Gate::Not, equal(word(actual), word(expected))));
    }
    append("(assert " + name(mismatch) + ")\n(check-sat)\n");
    return std::move(script_);
  }
};

enum class Width { Byte, Long, Quad };

std::string_view asm_reg(R r, Width width) {
  static constexpr std::array<std::string_view, register_count> bytes{
      "%al", "%cl", "%dl", "%bl", "%bpl", "%sil", "%dil",
      "%r8b", "%r9b", "%r10b", "%r11b", "%r12b", "%r13b", "%r14b", "%r15b"};
  static constexpr std::array<std::string_view, register_count> longs{
      "%eax", "%ecx", "%edx", "%ebx", "%ebp", "%esi", "%edi",
      "%r8d", "%r9d", "%r10d", "%r11d", "%r12d", "%r13d", "%r14d", "%r15d"};
  static constexpr std::array<std::string_view, register_count> quads{
      "%rax", "%rcx", "%rdx", "%rbx", "%rbp", "%rsi", "%rdi",
      "%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"};
  const auto n = index(r);
  return width == Width::Byte ? bytes[n] : width == Width::Long ? longs[n] : quads[n];
}

void render(std::ostream& out, const I& i, std::string_view helper) {
  const auto dst = asm_reg(i.dst, Width::Long);
  const auto src = asm_reg(i.src, Width::Long);
  const auto disp = [&] { out << i.integer << "(%rsp)"; };
  out << '\t';
  switch (i.op) {
    case O::SubSp: case O::AddSp:
      out << (i.op == O::SubSp ? "subq $" : "addq $") << i.integer << ", %rsp"; break;
    case O::Mov32: out << "movl " << src << ", " << dst; break;
    case O::Imm32: out << "movl $" << i.integer << ", " << dst; break;
    case O::Load32: out << "movl "; disp(); out << ", " << dst; break;
    case O::Store32: out << "movl " << src << ", "; disp(); break;
    case O::Save64: out << "movq " << asm_reg(i.src, Width::Quad) << ", "; disp(); break;
    case O::Restore64: out << "movq "; disp(); out << ", " << asm_reg(i.dst, Width::Quad); break;
    case O::Add32: out << "addl " << src << ", " << dst; break;
    case O::Sub32: out << "subl " << src << ", " << dst; break;
    case O::Mul32: out << "imull " << src << ", " << dst; break;
    case O::Compare32: out << "cmpl " << src << ", " << dst; break;
    case O::CompareZero32: out << "testl " << dst << ", " << dst; break;
    case O::SetEqual8: out << "sete " << asm_reg(i.dst, Width::Byte); break;
    case O::SetBelow8: out << "setb " << asm_reg(i.dst, Width::Byte); break;
    case O::MoveNonzero32: out << "cmovnel " << src << ", " << dst; break;
    case O::CallIdentity: out << "call " << helper; break;
    case O::Ret: out << "ret"; break;
    default: throw std::logic_error("unchecked native opcode reached renderer");
  }
  out << '\n';
}

std::string assembly(const NativeProgram& p, std::string_view symbol) {
  const auto helper = ".L" + std::string(symbol) + "_identity";
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "\t.text\n\t.globl " << symbol << "\n\t.type " << symbol
      << ", @function\n" << symbol << ":\n";
  for (const auto& i : p.prologue) render(out, i, helper);
  for (const auto& span : p.body)
    for (const auto& i : span.code) render(out, i, helper);
  for (const auto& i : p.epilogue) render(out, i, helper);
  out << "\t.size " << symbol << ", .-" << symbol << '\n';
  if (!p.identity_helper.empty()) {
    out << "\t.type " << helper << ", @function\n" << helper << ":\n";
    for (const auto& i : p.identity_helper) render(out, i, helper);
    out << "\t.size " << helper << ", .-" << helper << '\n';
  }
  out << "\t.section .note.GNU-stack,\"\",@progbits\n";
  return std::move(out).str();
}

NativeCheckResult verify(const ir::Function& f, const NativeProgram& p,
                         const NativeRequest& request, NativeLimits limits,
                         std::optional<std::string_view> submitted) {
  NativeCheckResult result;
  try {
    const auto& code = subject(f, request, limits);
    check_native_size(p, code.size(), limits);
    const auto slots = check_layout(code, p);
    check_liveness(code, p, request);
    check_boundary_code(p, slots);
    check_helper(p, request);
    Terms terms;
    bind_body(code, p, request, slots, terms);
    auto text = assembly(p, request.symbol);
    require(!submitted || text == *submitted, "assembly differs from the checked rendering");
    const auto budget = std::min(limits.max_smt_bytes, script_limit);
    result.smtlib2 = Circuit(terms, budget).encode();
    const auto solved = run_smtlib2(result.smtlib2);
    if (solved.status == ObligationStatus::SatCounterexample) {
      result.status = NativeStatus::Rejected;
      result.diagnostic = "modeled native code differs from the i32 IR: " + solved.diagnostic;
      return result;
    }
    result.assembly = std::move(text);
    result.binding_checked = true;
    result.status = solved.status == ObligationStatus::UnsatProved &&
                            solved.syntax == SMTSyntaxStatus::Valid
                        ? NativeStatus::Proved : NativeStatus::Unknown;
    result.diagnostic = solved.diagnostic;
  } catch (const Unsupported& error) {
    result.diagnostic = error.what();
  } catch (const std::invalid_argument& error) {
    result.status = NativeStatus::Rejected;
    result.diagnostic = error.what();
  }
  return result;
}

} // namespace

NativeLoweringResult lower_native(const ir::Function& f, const NativeRequest& request,
                                 NativeLimits limits) {
  try {
    const auto& code = subject(f, request, limits);
    auto program = generate(code, request);
    check_native_size(program, code.size(), limits);
    return {EvidenceStatus::ObligationReady, {}, std::move(program)};
  } catch (const Unsupported& error) {
    return {EvidenceStatus::Unknown, error.what(), std::nullopt};
  } catch (const std::invalid_argument& error) {
    return {EvidenceStatus::Rejected, error.what(), std::nullopt};
  }
}

NativeCheckResult verify_native(const ir::Function& f, const NativeProgram& p,
                                const NativeRequest& request, NativeLimits limits) {
  return verify(f, p, request, limits, std::nullopt);
}

NativeCheckResult verify_native_assembly(const ir::Function& f, const NativeProgram& p,
                                         std::string_view text, const NativeRequest& request,
                                         NativeLimits limits) {
  return verify(f, p, request, limits, text);
}

} // namespace novostone::pcc
