#include "cesk_theta.hpp"
#include "../../tests/test.hpp"

#include <array>

using namespace limestone;
namespace cesk = traceml::cesk_theta;
using enum cesk::Opcode;

namespace {

cesk::ExecutionOptions hot() {
  cesk::ExecutionOptions options;
  options.hot_threshold = 1;
  return options;
}

cesk::ExecutionOptions interpreter() {
  auto options = hot();
  options.hot_threshold = 0;
  return options;
}

void check_equivalence(const cesk::ExecutionResult& traced, const cesk::ExecutionResult& plain) {
  CHECK(traced.state == plain.state);
  CHECK(traced.steps == plain.steps);
  CHECK(traced.steps == traced.interpreted_steps + traced.traced_steps);
  CHECK(plain.interpreted_steps == plain.steps);
  CHECK(plain.traces.empty() && plain.side_exits.empty() && plain.traced_steps == 0);
}

const cesk::Program countdown{
  {Lambda, 4}, {Apply1}, {Print}, {Stop},
  // 4: decrement the captured parameter, then check the new value.
  {Load, 0}, {Constant, 1}, {Sub}, {Store, 0}, {Load, 0}, {Test}, {Branch, 4, 11},
  {Load, 0}, {Return},
};

void branch_side_exit() {
  for (cesk::Cell n = 1; n <= 96; ++n) {
    const std::array input{n};
    auto options = hot();
    size_t polls = 0;
    options.cancelled = [&] { ++polls; return false; };
    auto traced = take(cesk::execute(countdown, input, options));
    check_equivalence(traced, take(cesk::execute(countdown, input, interpreter())));
    CHECK(polls == traced.steps); // failed guard + fallback is one guest instruction
    CHECK(traced.state.output == 0 && traced.state.store == std::vector<cesk::Cell>{0});
    CHECK(traced.state.pc == 4 && traced.state.stack.empty());
    CHECK(traced.state.continuations.empty() && traced.state.environment.empty());
    if (n < 3) {
      CHECK(traced.traces.empty() && traced.traced_steps == 0);
      CHECK(traced.abandoned_recordings == static_cast<size_t>(n == 2));
      continue;
    }
    CHECK(traced.traces.size() == 1 && traced.traces[0].header == 4);
    const auto& path = traced.traces[0].steps;
    CHECK(path.size() == 7 && path.front().pc == 4 && path.back().successor == 4);
    CHECK(path.back().guard == cesk::Guard{cesk::GuardKind::Branch, 1});
    CHECK(traced.trace_entries == static_cast<size_t>(n - 2));
    CHECK(traced.completed_trace_iterations == static_cast<size_t>(n - 3));
    CHECK(traced.traced_steps == static_cast<size_t>(7 * (n - 3) + 6));
    CHECK(traced.side_exits == std::vector<cesk::SideExit>{
      {0, 10, static_cast<size_t>(7*n + 1), {cesk::GuardKind::Branch, 1}, 0}
    });
  }
  const std::array<cesk::Cell, 1> input{9};
  auto first = take(cesk::execute(countdown, input, hot()));
  auto second = take(cesk::execute(countdown, input, hot()));
  CHECK(first.traces == second.traces && first.side_exits == second.side_exits);
}

void call_target_side_exit() {
  const cesk::Program program{
    {Lambda, 4}, {Apply1}, {Print}, {Stop},
    // 4: sibling closures share the caller's parameter cell; discard their handles.
    {Lambda, 19}, {Lambda, 25}, {Print}, {Print},
    // 8: compute the dynamic callee handle 1 + (counter == 1).
    {Constant, 1}, {Load, 0}, {Constant, 1}, {Eq}, {Constant, 1}, {Add},
    {Apply1}, {Test}, {Branch, 8, 17}, {Load, 0}, {Return},
    // 19: decrement the captured counter and return it.
    {Load, 1}, {Load, 0}, {Sub}, {Store, 1}, {Load, 1}, {Return},
    // 25: modify the same captured counter and return false.
    {Constant, 42}, {Store, 1}, {Constant, 0}, {Return},
  };
  const std::array<cesk::Cell, 1> input{10};
  auto traced = take(cesk::execute(program, input, hot()));
  check_equivalence(traced, take(cesk::execute(program, input, interpreter())));
  CHECK(traced.state.output == 42);
  CHECK(traced.state.store == std::vector<cesk::Cell>({42, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));
  CHECK(traced.state.closures == std::vector<cesk::Closure>({{4, {}}, {19, {0}}, {25, {0}}}));
  CHECK(traced.completed_trace_iterations == 7 && traced.side_exits.size() == 1);
  const auto& exit = traced.side_exits.front();
  CHECK(exit.resume_pc == 14 && exit.guard == cesk::Guard{cesk::GuardKind::CallTarget, 19});
  CHECK(exit.actual == 25); // guard checked before allocating or popping the call
}

void return_target_side_exit() {
  const cesk::Program program{
    {Constant, 11}, {Lambda, 10}, {Apply1},
    {Constant, 31}, {Constant, 0}, {Apply1}, {Add}, {Stop},
    // 8 is reached by both calls, but their continuation PCs differ.
    {Load, 0}, {Return}, {Jump, 8},
  };
  auto traced = take(cesk::execute(program, {}, hot()));
  check_equivalence(traced, take(cesk::execute(program, {}, interpreter())));
  CHECK(traced.state.stack == std::vector<cesk::Cell>{42});
  CHECK(traced.traces.size() == 1 && traced.traces[0].header == 8);
  CHECK(traced.trace_entries == 1 && traced.traced_steps == 1);
  CHECK(traced.completed_trace_iterations == 0);
  CHECK(traced.side_exits == std::vector<cesk::SideExit>{
    {0, 9, 11, {cesk::GuardKind::ReturnTarget, 3}, 6}
  });
}

void a_call_guard_does_not_specialize_the_environment() {
  // The caller creates the same closure code on each iteration, but its
  // environment points to a freshly allocated argument cell. The trace must
  // use that new address, not the one seen by the recording.
  const cesk::Program program{
    {Constant, 0}, {Lambda, 5}, {Apply2}, {Print}, {Stop},
    // 5: bind factory in lexical slot 1, preserving the counter in slot 0.
    {Lambda, 18}, {Store, 1},
    // 7: call factory(counter), then call its returned closure with dummy 0.
    {Constant, 0}, {Load, 0}, {Load, 1}, {Apply1}, {Apply1}, {Print},
    {Load, 0}, {Test}, {Branch, 7, 16}, {Load, 0}, {Return},
    // 18: factory's child captures both main's counter and the factory argument.
    {Lambda, 20}, {Return},
    // 20: decrement the counter using the factory argument (not a stale one).
    {Load, 1}, {Constant, 1}, {Sub}, {Store, 2}, {Load, 1}, {Return},
  };
  const std::array<cesk::Cell, 1> input{8};
  auto traced = take(cesk::execute(program, input, hot()));
  check_equivalence(traced, take(cesk::execute(program, input, interpreter())));
  CHECK(traced.completed_trace_iterations == 5);
  CHECK(traced.side_exits.size() == 1 && traced.side_exits[0].guard.kind == cesk::GuardKind::Branch);
  CHECK(traced.state.store == std::vector<cesk::Cell>({0, 1, 8, 0, 7, 0, 6, 0, 5, 0, 4, 0, 3, 0, 2, 0, 1, 0}));
}

void arity_and_arithmetic() {
  const cesk::Program apply2{
    {Lambda, 3}, {Apply2}, {Stop}, {Load, 0}, {Load, 1}, {Sub}, {Return},
  };
  const std::array<cesk::Cell, 3> pair{99, 44, 2};
  auto two = take(cesk::execute(apply2, pair));
  CHECK(two.state.stack == std::vector<cesk::Cell>({99, 42}));
  CHECK(two.state.store == std::vector<cesk::Cell>({44, 2}));

  const cesk::Program apply3{
    {Lambda, 3}, {Apply3}, {Stop}, {Load, 0}, {Load, 2}, {Add}, {Load, 1}, {Sub}, {Return},
  };
  const std::array<cesk::Cell, 3> triple{50, 10, 2};
  auto three = take(cesk::execute(apply3, triple));
  CHECK(three.state.stack == std::vector<cesk::Cell>{42});
  CHECK(three.state.store == std::vector<cesk::Cell>({50, 10, 2}));

  constexpr auto low = std::numeric_limits<cesk::Cell>::min(), high = std::numeric_limits<cesk::Cell>::max();
  const cesk::Program arithmetic{
    {Constant, high}, {Constant, 1}, {Add}, {Neg}, {Constant, 1}, {Sub},
    {Constant, 2}, {Mul}, {Constant, 3}, {Div}, {Stop},
  };
  CHECK(take(cesk::execute(arithmetic)).state.stack == std::vector<cesk::Cell>{0});
  CHECK(take(cesk::execute({{Constant, low}, {Neg}, {Stop}})).state.stack == std::vector<cesk::Cell>{low});
  for (const auto [op, expected] : std::array{
    std::pair{Eq, 0}, std::pair{Ne, 1}, std::pair{Lt, 1},
    std::pair{Le, 1}, std::pair{Gt, 0}, std::pair{Ge, 0},
  }) {
    CHECK(take(cesk::execute({{Constant, -1}, {Constant, 0}, {op}, {Stop}}))
          .state.stack == std::vector<cesk::Cell>{expected});
  }
}

void verification_and_budgets() {
  for (const cesk::Program& malformed : std::array{
    cesk::Program{}, cesk::Program{{static_cast<cesk::Opcode>(255)}},
    cesk::Program{{Branch, 0, 1}}, cesk::Program{{Jump, -1}},
    cesk::Program{{Lambda, 1}}, cesk::Program{{Load, -1}}, cesk::Program{{Store, 64}},
    cesk::Program{{Stop, 1}}, cesk::Program{{Constant, 0, 1}},
  }) {
    fails(cesk::verify(malformed), Error::Code::InvalidArgument);
    fails(cesk::execute(malformed), Error::Code::InvalidArgument);
  }
  for (const cesk::Program& invalid_run : std::array{
    cesk::Program{{Constant, 1}}, cesk::Program{{Load, 0}, {Stop}},
    cesk::Program{{Constant, 5}, {Constant, 0}, {Apply1}, {Stop}},
    cesk::Program{{Return}}, cesk::Program{{Add}, {Stop}},
    cesk::Program{{Constant, 1}, {Constant, 0}, {Div}, {Stop}},
    cesk::Program{{Constant, std::numeric_limits<cesk::Cell>::min()}, {Constant, -1}, {Div}, {Stop}},
  }) {
    take(cesk::verify(invalid_run));
    fails(cesk::execute(invalid_run), Error::Code::InvalidArgument);
  }
  // A callee cannot accidentally use its caller's operand stack.
  const std::array<cesk::Cell, 2> frame_input{99, 42};
  fails(cesk::execute({{Lambda, 3}, {Apply1}, {Stop}, {Return}}, frame_input), Error::Code::InvalidArgument);
  const std::vector<cesk::Cell> full_stack(513);
  fails(cesk::execute({{Stop}}, full_stack), Error::Code::ResourceLimit);

  const std::array<cesk::Cell, 1> input{12};
  auto truncated = hot();
  truncated.max_trace_length = 6;
  auto abandoned = take(cesk::execute(countdown, input, truncated));
  CHECK(abandoned.abandoned_recordings == 1 && abandoned.traces.empty());
  check_equivalence(abandoned, take(cesk::execute(countdown, input, interpreter())));
  for (auto limit : {&cesk::ExecutionOptions::max_trace_length, &cesk::ExecutionOptions::max_traces}) {
    auto disabled = hot();
    disabled.*limit = 0;
    auto plain = take(cesk::execute(countdown, input, disabled));
    CHECK(plain.interpreted_steps == plain.steps && plain.abandoned_recordings == 0);
  }

  const cesk::Program infinite{{Jump, 0}};
  auto limited = hot();
  limited.step_limit = 8;
  size_t calls = 0;
  limited.cancelled = [&] { ++calls; return false; };
  fails(cesk::execute(infinite, {}, limited), Error::Code::ResourceLimit);
  CHECK(calls == 8);
  calls = 0;
  limited.cancelled = [&] { return ++calls == 5; };
  fails(cesk::execute(infinite, {}, limited), Error::Code::Interrupted);
  CHECK(calls == 5);
}

void multiple_hot_headers() {
  auto program = countdown;
  program.resize(11);
  program.insert(program.end(), {
    {Constant, 6}, {Store, 0},
    {Load, 0}, {Constant, 1}, {Sub}, {Store, 0}, {Load, 0}, {Test}, {Branch, 13, 20},
    {Load, 0}, {Return},
  });
  const std::array<cesk::Cell, 1> input{8};
  const auto plain = take(cesk::execute(program, input, interpreter()));
  auto options = cesk::ExecutionOptions{};
  options.max_trace_length = 7; // exact fit
  options.step_limit = plain.steps;
  auto both = take(cesk::execute(program, input, options));
  check_equivalence(both, plain);
  CHECK(both.traces.size() == 2 && both.traces[0].header == 4 && both.traces[1].header == 13);
  CHECK(both.completed_trace_iterations == 4);
  CHECK(both.side_exits.size() == 2 && both.side_exits[0].trace_index == 0 && both.side_exits[1].trace_index == 1);
  options.max_traces = 1;
  auto one = take(cesk::execute(program, input, options));
  check_equivalence(one, plain);
  CHECK(one.traces.size() == 1 && one.traces[0].header == 4 && one.side_exits.size() == 1);
  --options.step_limit;
  fails(cesk::execute(program, input, options), Error::Code::ResourceLimit);
}

void errors_from_live_trace_state() {
  const cesk::Program program{
    {Lambda, 3}, {Apply1}, {Stop},
    {Constant, 1}, {Load, 0}, {Sub}, {Constant, 7}, {Load, 0}, {Div}, {Print},
    {Neg}, {Store, 0}, {Jump, 3},
  };
  const std::array<cesk::Cell, 1> input{3};
  auto traced = cesk::execute(program, input, hot());
  auto plain = cesk::execute(program, input, interpreter());
  fails(traced, Error::Code::InvalidArgument);
  fails(plain, Error::Code::InvalidArgument);
  CHECK(traced.error().message == plain.error().message);
  CHECK(traced.error().message.find("pc 8: invalid signed division") != std::string::npos);

  const cesk::Program allocates{{Lambda, 2}, {Jump, 0}, {Return}};
  traced = cesk::execute(allocates, {}, hot());
  plain = cesk::execute(allocates, {}, interpreter());
  fails(traced, Error::Code::ResourceLimit);
  fails(plain, Error::Code::ResourceLimit);
  CHECK(traced.error().message == plain.error().message);
  CHECK(traced.error().message.find("closure limit") != std::string::npos);
}

} // namespace

int main() {
  return test_main([] {
    branch_side_exit();
    call_target_side_exit();
    return_target_side_exit();
    a_call_guard_does_not_specialize_the_environment();
    arity_and_arithmetic();
    verification_and_budgets();
    multiple_hot_headers();
    errors_from_live_trace_state();
  });
}
