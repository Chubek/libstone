#include "cesk_theta.hpp"

#include <bit>
#include <limits>
#include <map>

namespace limestone::traceml::cesk_theta {
namespace {

constexpr size_t stack_limit = 512, store_limit = 256, environment_limit = 64;
constexpr size_t closure_limit = 64, continuation_limit = 128;

[[noreturn]] void fail(size_t pc, const char* message, Error::Code code = Error::Code::InvalidArgument) {
  throw Error{code, "CESK-Theta pc " + std::to_string(pc) + ": " + message};
}

constexpr bool is_apply(Opcode op) {
  return op == Opcode::Apply1 || op == Opcode::Apply2 || op == Opcode::Apply3;
}

constexpr bool is_loop_edge(Opcode op, size_t from, size_t to) {
  return (op == Opcode::Jump || op == Opcode::Branch) && to <= from;
}

constexpr std::optional<GuardKind> guard_kind(Opcode op) {
  if (op == Opcode::Branch) return GuardKind::Branch;
  if (is_apply(op)) return GuardKind::CallTarget;
  if (op == Opcode::Return) return GuardKind::ReturnTarget;
  return std::nullopt;
}

void verify_tape(std::span<const Instruction> tape) {
  if (tape.empty()) fail(0, "empty instruction tape");
  for (size_t pc = 0; pc < tape.size(); ++pc) {
    const auto [op, operand, alternative] = tape[pc];
    const auto target = [&](Cell value) {
      if (value < 0 || static_cast<uint64_t>(value) >= tape.size())
        fail(pc, "control target outside instruction tape");
    };
    if (static_cast<unsigned>(op) > static_cast<unsigned>(Opcode::Stop))
      fail(pc, "unknown opcode");
    if (op != Opcode::Branch && alternative != 0)
      fail(pc, "unexpected alternative operand");
    switch (op) {
      case Opcode::Branch: target(alternative); [[fallthrough]];
      case Opcode::Jump:
      case Opcode::Lambda: target(operand); break;
      case Opcode::Constant: break;
      case Opcode::Load:
      case Opcode::Store:
        if (operand < 0 || static_cast<uint64_t>(operand) >= environment_limit)
          fail(pc, "invalid lexical index");
        break;
      default:
        if (operand != 0) fail(pc, "unexpected instruction operand");
    }
  }
}

size_t stack_inputs(const MachineState& state) {
  return state.stack.size() -
    (state.continuations.empty() ? 0 : state.continuations.back().stack_base);
}

void require_inputs(const MachineState& state, size_t count) {
  if (stack_inputs(state) < count) fail(state.pc, "operand stack underflow");
}

size_t store_address(const MachineState& state, size_t index) {
  if (index >= state.environment.size()) fail(state.pc, "unbound lexical index");
  // Only Apply creates bindings; Restore and Lambda only copy those addresses.
  return state.environment[state.environment.size() - 1 - index];
}

void push(MachineState& state, Cell value) {
  if (state.stack.size() == stack_limit)
    fail(state.pc, "operand stack limit reached", Error::Code::ResourceLimit);
  state.stack.push_back(value);
}

struct Transition {
  size_t successor;
  std::optional<Guard> observed = std::nullopt;
  bool side_exit = false;
};

using Handler = Transition (*)(MachineState&, const Instruction&, const TraceStep*);

// One semantic kernel for both tiers. Each Op is specialized once by C++; the
// interpreter selects it from the tape opcode, replay uses prebound handlers.
// A replay guard MUST be checked before any effect of that guest instruction.
template<Opcode Op>
Transition transition(MachineState& state, const Instruction& ins, const TraceStep* replay) {
  const auto next = state.pc + 1;
  auto edge = [replay](size_t successor, Guard observed) -> Transition {
    if (!replay) return {successor, observed};
    if (replay->guard->expected != observed.expected) return {successor, observed, true};
    return {replay->successor};
  };
  if constexpr (Op == Opcode::Constant) {
    push(state, ins.operand);
  } else if constexpr (Op == Opcode::Load) {
    push(state, state.store[store_address(state, static_cast<size_t>(ins.operand))]);
  } else if constexpr (Op == Opcode::Store) {
    require_inputs(state, 1);
    state.store[store_address(state, static_cast<size_t>(ins.operand))] = state.stack.back();
    state.stack.pop_back();
  } else if constexpr (Op == Opcode::Lambda) {
    if (state.closures.size() == closure_limit)
      fail(state.pc, "closure limit reached", Error::Code::ResourceLimit);
    push(state, static_cast<Cell>(state.closures.size()));
    state.closures.push_back({static_cast<size_t>(ins.operand), state.environment});
  } else if constexpr (is_apply(Op)) {
    constexpr size_t count = Op == Opcode::Apply1 ? 1 : Op == Opcode::Apply2 ? 2 : 3;
    require_inputs(state, count + 1);
    const auto handle = state.stack.back();
    if (handle < 0 || static_cast<uint64_t>(handle) >= state.closures.size())
      fail(state.pc, "invalid closure handle");
    const auto& closure = state.closures[static_cast<size_t>(handle)];
    const auto path = edge(closure.entry_pc, {GuardKind::CallTarget, closure.entry_pc});
    if (path.side_exit) return path;
    if (state.store.size() + count > store_limit)
      fail(state.pc, "store limit reached", Error::Code::ResourceLimit);
    if (state.continuations.size() == continuation_limit)
      fail(state.pc, "continuation limit reached", Error::Code::ResourceLimit);
    if (closure.environment.size() + count > environment_limit)
      fail(state.pc, "environment limit reached", Error::Code::ResourceLimit);

    const auto base = state.stack.size() - count - 1;
    const auto first_address = state.store.size();
    auto captured = closure.environment;
    // Push in reverse argument order so argument 0 is the newest binding.
    for (size_t i = count; i > 0; --i) captured.push_back(first_address + i - 1);
    for (size_t i = 0; i < count; ++i) state.store.push_back(state.stack[base + i]);
    state.continuations.push_back({next, base, std::move(state.environment)});
    state.environment = std::move(captured);
    state.stack.resize(base);
    return path;
  } else if constexpr (Op == Opcode::Return) {
    if (state.continuations.empty()) fail(state.pc, "return without a continuation");
    require_inputs(state, 1);
    auto& frame = state.continuations.back();
    const auto path = edge(frame.return_pc, {GuardKind::ReturnTarget, frame.return_pc});
    if (path.side_exit) return path;
    const auto value = state.stack.back();
    state.stack.resize(frame.stack_base + 1);
    state.stack.back() = value;
    state.environment = std::move(frame.environment);
    state.continuations.pop_back();
    return path;
  } else if constexpr (Op == Opcode::Test || Op == Opcode::Print || Op == Opcode::Neg) {
    require_inputs(state, 1);
    const auto value = state.stack.back();
    if constexpr (Op == Opcode::Neg) {
      state.stack.back() = std::bit_cast<Cell>(uint64_t{0} - static_cast<uint64_t>(value));
    } else {
      if constexpr (Op == Opcode::Test) state.flag = value != 0;
      else state.output = value;
      state.stack.pop_back();
    }
  } else if constexpr (Op == Opcode::Branch) {
    return edge(static_cast<size_t>(state.flag ? ins.operand : ins.alternative),
                {GuardKind::Branch, static_cast<size_t>(state.flag)});
  } else if constexpr (Op == Opcode::Jump) {
    return {replay ? replay->successor : static_cast<size_t>(ins.operand)};
  } else if constexpr (Op == Opcode::Stop) {
    return {next};
  } else {
    require_inputs(state, 2);
    const auto a = state.stack[state.stack.size() - 2], b = state.stack.back();
    const auto ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
    Cell result;
    if constexpr (Op == Opcode::Add) result = std::bit_cast<Cell>(ua + ub);
    else if constexpr (Op == Opcode::Sub) result = std::bit_cast<Cell>(ua - ub);
    else if constexpr (Op == Opcode::Mul) result = std::bit_cast<Cell>(ua * ub);
    else if constexpr (Op == Opcode::Div) {
      if (b == 0 || (a == std::numeric_limits<Cell>::min() && b == -1))
        fail(state.pc, "invalid signed division");
      result = a / b;
    } else if constexpr (Op == Opcode::Eq) result = a == b;
    else if constexpr (Op == Opcode::Ne) result = a != b;
    else if constexpr (Op == Opcode::Lt) result = a < b;
    else if constexpr (Op == Opcode::Le) result = a <= b;
    else if constexpr (Op == Opcode::Gt) result = a > b;
    else if constexpr (Op == Opcode::Ge) result = a >= b;
    state.stack.pop_back();
    state.stack.back() = result;
  }
  return {replay ? replay->successor : next};
}

Handler dispatch(Opcode opcode) {
  switch (opcode) {
#define CESK_HANDLER(op) case Opcode::op: return transition<Opcode::op>
    CESK_HANDLER(Constant); CESK_HANDLER(Load); CESK_HANDLER(Store); CESK_HANDLER(Lambda);
    CESK_HANDLER(Apply1); CESK_HANDLER(Apply2); CESK_HANDLER(Apply3); CESK_HANDLER(Return);
    CESK_HANDLER(Test); CESK_HANDLER(Branch); CESK_HANDLER(Jump);
    CESK_HANDLER(Add); CESK_HANDLER(Sub); CESK_HANDLER(Mul); CESK_HANDLER(Div); CESK_HANDLER(Neg);
    CESK_HANDLER(Eq); CESK_HANDLER(Ne); CESK_HANDLER(Lt); CESK_HANDLER(Le);
    CESK_HANDLER(Gt); CESK_HANDLER(Ge); CESK_HANDLER(Print); CESK_HANDLER(Stop);
#undef CESK_HANDLER
  }
  throw Error{Error::Code::Internal, "CESK-Theta opcode was not verified"};
}

// Compile a closed recorded path to a predecoded array of handlers. Guards
// are mandatory for exactly the control transfers that depend on red state.
// Keep verification here so a recorder bug cannot install a partial trace.
std::vector<Handler> compile_loop(const LoopTrace& trace, std::span<const Instruction> tape) {
  std::vector<Handler> code;
  code.reserve(trace.steps.size());
  size_t pc = trace.header;
  for (const auto& step : trace.steps) {
    const auto op = step.instruction.opcode;
    const auto kind = guard_kind(op);
    if (pc >= tape.size() || step.pc != pc || step.instruction != tape[pc] ||
        step.successor >= tape.size() || op == Opcode::Stop ||
        kind != (step.guard ? std::optional{step.guard->kind} : std::nullopt))
      fail(pc, "invalid recorded trace control edge", Error::Code::Internal);
    size_t expected = pc + 1;
    if (op == Opcode::Jump) expected = static_cast<size_t>(step.instruction.operand);
    if (op == Opcode::Branch) {
      if (step.guard->expected > 1) fail(pc, "invalid branch guard", Error::Code::Internal);
      expected = static_cast<size_t>(
        step.guard->expected ? step.instruction.operand : step.instruction.alternative);
    } else if (kind) {
      expected = step.guard->expected;
    }
    if (step.successor != expected)
      fail(pc, "invalid recorded trace successor", Error::Code::Internal);
    pc = step.successor;
    code.push_back(dispatch(op));
  }
  if (code.empty() || pc != trace.header)
    fail(trace.header, "trace does not close at its header", Error::Code::Internal);
  return code;
}

struct Header {
  size_t hits = 0;
  std::optional<size_t> trace_index;
  bool attempted = false;
};

ExecutionResult run(std::span<const Instruction> tape, std::span<const Cell> input,
                    const ExecutionOptions& options) {
  ExecutionResult result;
  auto& state = result.state;
  if (input.size() > stack_limit) fail(0, "initial stack exceeds limit", Error::Code::ResourceLimit);
  state.stack.assign(input.begin(), input.end());

  const bool tracing = options.hot_threshold && options.max_trace_length && options.max_traces;
  std::map<size_t, Header> headers;
  std::optional<LoopTrace> recording;
  std::vector<std::vector<Handler>> cache; // Parallel to result.traces.
  std::optional<size_t> active_trace;
  size_t cursor = 0;

  for (;;) {
    const auto pc = state.pc;
    if (pc >= tape.size()) fail(pc, "fell off instruction tape without Stop");
    if (result.steps == options.step_limit)
      fail(pc, "execution step limit reached", Error::Code::ResourceLimit);
    if (options.cancelled && options.cancelled())
      fail(pc, "execution cancelled", Error::Code::Interrupted);

    if (!active_trace && !recording && tracing) {
      if (const auto it = headers.find(pc); it != headers.end() && it->second.trace_index) {
        active_trace = it->second.trace_index;
        cursor = 0;
        ++result.trace_entries;
      }
    }
    if (active_trace) {
      const auto index = *active_trace;
      const auto& step = result.traces[index].steps[cursor];
      const auto outcome = cache[index][cursor](state, step.instruction, &step);
      if (!outcome.side_exit) {
        state.pc = outcome.successor;
        ++result.steps;
        ++result.traced_steps;
        if (++cursor == cache[index].size()) {
          ++result.completed_trace_iterations;
          active_trace.reset();
        }
        continue;
      }
      result.side_exits.push_back({index, pc, result.steps, *step.guard, outcome.observed->expected});
      active_trace.reset();
      // The guard did not commit this instruction. Fall through to the
      // interpreter once at the same pc, without rechecking the guest budget.
    }

    const auto& ins = tape[pc];
    const auto outcome = dispatch(ins.opcode)(state, ins, nullptr);
    state.pc = outcome.successor;
    ++result.steps;
    ++result.interpreted_steps;
    if (ins.opcode == Opcode::Stop) {
      if (recording) ++result.abandoned_recordings;
      return result;
    }

    if (recording) {
      recording->steps.push_back({pc, state.pc, ins, outcome.observed});
      if (state.pc == recording->header) {
        cache.push_back(compile_loop(*recording, tape));
        headers.at(recording->header).trace_index = result.traces.size();
        result.traces.push_back(std::move(*recording));
        recording.reset();
      } else if (recording->steps.size() == options.max_trace_length) {
        ++result.abandoned_recordings;
        recording.reset();
      }
    } else if (tracing && cache.size() < options.max_traces &&
               is_loop_edge(ins.opcode, pc, state.pc)) {
      auto& header = headers[state.pc];
      // Each header gets one recording attempt. A path that cannot close
      // within the budget remains interpreted rather than repeatedly retracing.
      if (!header.attempted && ++header.hits == options.hot_threshold) {
        header.attempted = true;
        recording = LoopTrace{state.pc, {}};
      }
    }
  }
}
} // namespace

Result<int> verify(std::span<const Instruction> tape) {
  try {
    verify_tape(tape);
    return Result<int>::ok(0);
  } catch (const Error& error) {
    return Result<int>::err(error);
  }
}

Result<ExecutionResult> execute(const Program& tape, std::span<const Cell> input,
                                const ExecutionOptions& options) {
  try {
    // A synchronous cancellation callback cannot invalidate a running tape
    // or its policy by editing the caller's copies.
    const auto code = tape;
    const auto policy = options;
    verify_tape(code);
    return Result<ExecutionResult>::ok(run(code, input, policy));
  } catch (const Error& error) {
    return Result<ExecutionResult>::err(error);
  } catch (const std::bad_alloc&) {
    return Result<ExecutionResult>::err({Error::Code::ResourceLimit, "CESK-Theta allocation failed"});
  } catch (const std::exception& error) {
    return Result<ExecutionResult>::err({Error::Code::Internal,
      std::string("CESK-Theta execution: ") + error.what()});
  } catch (...) {
    return Result<ExecutionResult>::err({Error::Code::Internal, "CESK-Theta execution exception"});
  }
}

} // namespace limestone::traceml::cesk_theta
