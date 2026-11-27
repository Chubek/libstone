#include "LibDSL.hpp"

#include <cassert>

using namespace dsl::synth;

namespace
{
struct ProgramError { int code; };
struct InstrumentationError {};
struct Program
{
  bool fail;
  void run (int &state, int limit) const
  {
    state += limit;
    if (fail)
      throw ProgramError{ limit };
  }
};

template <class Error, class Action>
Error expect (Action action)
{
  try
    {
      action ();
    }
  catch (const Error &error)
    {
      return error;
    }
  throw std::logic_error ("missing expected exception");
}
}

int main ()
{
  int state = 2, captures = 0, restores = 0, exceptional = 0, normal = 0;
  auto capture = [&] (const int &value) { ++captures; return value; };
  auto restore = [&] (int &value, const int &before) noexcept {
    ++restores;
    value = before;
  };
  ExecutionContract<int> contract;
  assert (expect<ProgramError> ([&] { contract.run (Program{ true }, state, capture, 3); }).code == 3);
  assert (state == 5 && captures == 1 && restores == 0);

  state = 2;
  contract.require (ContractPhase::postcondition, "normal result",
                    [&] (int before, int after) {
                      ++normal;
                      return before == 2 && after == 5;
                    });
  contract.require_exceptional ("expected program exception",
      [&] (int before, int after, const std::exception_ptr &error) {
        ++exceptional;
        const auto cause = expect<ProgramError> ([&] { std::rethrow_exception (error); });
        return before == 2 && after == 5 && cause.code == 3;
      });
  const auto clauses = contract.obligations ();
  assert (clauses.size () == 2 && clauses.back ().phase == ContractPhase::exceptional_exit);
  assert (exceptional == 0 && normal == 0);

  assert (expect<ProgramError> ([&] {
    contract.run_transactional (Program{ true }, state, capture, 3, restore);
  }).code == 3);
  assert (state == 2 && restores == 1 && exceptional == 1 && normal == 0);

  const auto violation = expect<ExceptionalContractViolation> ([&] {
    contract.run_transactional (Program{ true }, state, capture, 4, restore);
  });
  assert (expect<ProgramError> ([&] { std::rethrow_exception (violation.outcome ()); }).code == 4);
  assert (state == 2 && restores == 2 && exceptional == 2 && normal == 0);

  expect<std::runtime_error> ([&] {
    contract.run_transactional (Program{ false }, state, capture, 4, restore);
  });
  assert (state == 2 && restores == 3 && exceptional == 2 && normal == 1);

  contract.run_transactional (Program{ false }, state, capture, 3, restore);
  assert (state == 5 && restores == 3 && exceptional == 2 && normal == 2);

  state = 2;
  auto fail_after = [] (const int &value) {
    if (value == 5)
      throw InstrumentationError{};
    return value;
  };
  for (bool fail : { false, true })
    expect<InstrumentationError> ([&] {
      contract.run_transactional (Program{ fail }, state, fail_after, 3, restore);
    });
  assert (state == 2 && restores == 5 && exceptional == 2 && normal == 2);

  auto fail_before = [] (const int &) -> int { throw InstrumentationError{}; };
  expect<InstrumentationError> ([&] {
    contract.run_transactional (Program{ false }, state, fail_before, 3, restore);
  });
  assert (state == 2 && restores == 5);

  ExecutionContract<int> boundary;
  boundary.require (ContractPhase::precondition, "reject before execution",
                    [] (int, int) { return false; });
  boundary.require_exceptional ("never reached",
      [] (int, int, const std::exception_ptr &) -> bool { throw InstrumentationError{}; });
  expect<std::runtime_error> ([&] {
    boundary.run_transactional (Program{ false }, state, capture, 3, restore);
  });
  assert (state == 2 && restores == 6);

  ExecutionContract<int> checker_failure;
  checker_failure.require_exceptional ("checker throws",
      [] (int, int, const std::exception_ptr &) -> bool { throw InstrumentationError{}; });
  expect<InstrumentationError> ([&] {
    checker_failure.run_transactional (Program{ true }, state, capture, 3, restore);
  });
  assert (state == 2 && restores == 7);
}
