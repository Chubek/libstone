#pragma once

#include "../limestone/foundation.hpp"

namespace limestone::traceml::cesk_theta {

// Typed instruction tape, not TraceLambda: closures and store cells are
// explicit. Targets are absolute instruction indices.
using Cell = int64_t;
enum class Opcode : uint8_t {
  Constant, Load, Store, Lambda, Apply1, Apply2, Apply3, Return,
  Test, Branch, Jump, Add, Sub, Mul, Div, Neg, Eq, Ne, Lt, Le, Gt, Ge, Print, Stop
};

struct Instruction {
  Opcode opcode = Opcode::Stop;
  Cell operand = 0;     // Constant value, lexical index, or taken/jump/lambda target.
  Cell alternative = 0; // Branch's false target only.
  bool operator==(const Instruction&) const = default;
};

using Program = std::vector<Instruction>;
Result<int> verify(std::span<const Instruction>);

struct Closure {
  size_t entry_pc = 0;
  std::vector<size_t> environment; // Store addresses, not captured values.
  bool operator==(const Closure&) const = default;
};

struct Continuation {
  size_t return_pc = 0, stack_base = 0;
  std::vector<size_t> environment;
  bool operator==(const Continuation&) const = default;
};

struct MachineState {
  size_t pc = 0; // Next instruction, including after Stop.
  std::vector<size_t> environment; // Newest binding last.
  std::vector<Cell> store;
  std::vector<Continuation> continuations;
  std::vector<Closure> closures;
  std::vector<Cell> stack;
  bool flag = false;
  Cell output = 0; // Last Print; initially zero.
  bool operator==(const MachineState&) const = default;
};

enum class GuardKind { Branch, CallTarget, ReturnTarget };
struct Guard {
  GuardKind kind = GuardKind::Branch;
  size_t expected = 0; // 0/1 for Branch; absolute pc otherwise.
  bool operator==(const Guard&) const = default;
};

struct TraceStep {
  size_t pc = 0, successor = 0;
  Instruction instruction;
  std::optional<Guard> guard;
  bool operator==(const TraceStep&) const = default;
};

struct LoopTrace {
  size_t header = 0;
  std::vector<TraceStep> steps;
  bool operator==(const LoopTrace&) const = default;
};

struct SideExit {
  size_t trace_index = 0;
  size_t resume_pc = 0; // Interpreter resumes BEFORE this instruction.
  size_t steps = 0;     // Total guest instructions already committed.
  Guard guard;
  size_t actual = 0;   // Same encoding as guard.expected.
  bool operator==(const SideExit&) const = default;
};

struct ExecutionOptions {
  size_t step_limit = 100000;
  size_t hot_threshold = 3;     // Interpreted backward arrivals before recording.
  size_t max_trace_length = 256;
  size_t max_traces = 64;
  // Setting any trace limit or the hot threshold to zero disables metatracing.
  // Called once before each guest instruction, regardless of execution tier.
  std::function<bool()> cancelled;
};

struct ExecutionResult {
  MachineState state;
  size_t steps = 0, interpreted_steps = 0, traced_steps = 0;
  size_t trace_entries = 0, completed_trace_iterations = 0, abandoned_recordings = 0;
  std::vector<LoopTrace> traces;
  std::vector<SideExit> side_exits;
};

// Starts at pc 0 with an empty CESK state except for the supplied operand stack
// (bottom first). The tape and trace cache are private to this invocation.
Result<ExecutionResult> execute(const Program&, std::span<const Cell> initial_stack = {},
                                const ExecutionOptions& = {});

} // namespace limestone::traceml::cesk_theta
