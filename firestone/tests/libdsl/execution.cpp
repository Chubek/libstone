#include "../../examples/libdsl/reference.hpp"

namespace
{
void
check (bool condition)
{
  if (!condition)
    throw std::logic_error ("W12 test assertion failed");
}

template <class Error, class Action>
void
reject (Action action)
{
  try
    {
      action ();
    }
  catch (const Error &)
    {
      return;
    }
  throw std::logic_error ("W12 expected exception was not thrown");
}
} // namespace

int
main ()
{
  using namespace dsl;
  using namespace dsl::synth;
  using namespace dsl::synth::reference;
  const auto numbers = arithmetic ();
  ArithmeticState empty;
  check (numbers.compile_expression ("(+ (* 6 7) (- 4 4))").run (empty) == 42);
  check (numbers.compile_expression ("-9223372036854775808").run (empty)
         == std::numeric_limits<std::int64_t>::min ());
  check (numbers.compile_expression ("9223372036854775807").run (empty)
         == std::numeric_limits<std::int64_t>::max ());
  reject<std::overflow_error> ([&] { numbers.compile_expression ("9223372036854775808"); });
  reject<std::overflow_error> ([&] { numbers.compile_expression ("-9223372036854775809"); });
  reject<std::overflow_error> ([&] {
    numbers.compile_expression ("(+ 9223372036854775807 1)").run (empty);
  });
  reject<std::overflow_error> ([&] {
    numbers.compile_expression ("(- -9223372036854775808 1)").run (empty);
  });
  reject<std::overflow_error> ([&] {
    numbers.compile_expression ("(* -1 -9223372036854775808)").run (empty);
  });
  reject<std::overflow_error> ([&] {
    numbers.compile_expression ("(/ -9223372036854775808 -1)").run (empty);
  });
  reject<std::domain_error> ([&] { numbers.compile_expression ("(/ 1 0)").run (empty); });
  reject<std::invalid_argument> ([&] { numbers.compile_expression ("(+ 1)"); });
  reject<std::invalid_argument> ([&] { numbers.compile_expression ("(exec 1)"); });
  reject<std::invalid_argument> ([&] {
    numbers.compile_expression (SExpr{ true, "3", { SExpr::atom ("1") }, false });
  });
  reject<std::invalid_argument> ([&] {
    numbers.compile_expression ("(+ 1 2)", { .syntax_nodes = 3 });
  });
  reject<std::invalid_argument> ([&] {
    numbers.compile_expression ("(+ 1 2)", { .syntax_depth = 1 });
  });
  reject<std::invalid_argument> ([&] {
    numbers.compile_expression ("42", { .source_bytes = 1 });
  });
  reject<std::runtime_error> ([&] {
    numbers.compile_expression ("(+ 1 2)").run (empty, { .execution_depth = 1 });
  });

  const MetaLanguage macro_text (Specification::from_text (
      "(language Arithmetic (form + 2) (macro (twice $x) (+ $x $x)))"));
  auto macro_native = SpecBuilder ("Arithmetic");
  macro_native.form ("+", 2)
      .macro (SExpr::list ({ SExpr::atom ("twice"), SExpr::atom ("$x") }),
              SExpr::list ({ SExpr::atom ("+"), SExpr::atom ("$x"), SExpr::atom ("$x") }));
  const MetaLanguage macro_built (macro_native.build ());
  check (macro_text.specification ().to_sexpr () == macro_built.specification ().to_sexpr ());
  const auto use_macro = parse_sexpr ("(twice 21)");
  check (numbers.compile_expanded_expression (macro_text, "(twice 21)").run (empty) == 42);
  check (numbers.compile_expanded_expression (macro_built, use_macro).run (empty) == 42);
  reject<std::invalid_argument> ([&] {
    numbers.compile_expanded_expression (macro_built, use_macro, { .syntax_nodes = 3 });
  });

  GuardedTermOptimizer optimizer;
  const auto verify_add_zero = [] (const TermRule &rule) {
    // A scoped, host-proved fact for this reference language: adding exact zero
    // cannot overflow, has no state effect, and preserves evaluation of ?x.
    return rule.lhs == parse_term ("(+ ?x 0)") && rule.rhs == parse_term ("?x");
  };
  optimizer.add_rule ("(rule add-zero (+ ?x 0) ?x)", verify_add_zero);
  const auto source = parse_sexpr ("(+ (* 6 7) 0)");
  const auto reduced = optimizer.optimize (source);
  check (reduced == parse_sexpr ("(* 6 7)"));
  check (numbers.compile_expression (reduced).run (empty)
         == numbers.compile_expression (source).run (empty));
  reject<std::invalid_argument> ([&] {
    optimizer.add_rule ("(rule erase (/ ?x 0) ?x)", verify_add_zero);
  });
  reject<std::invalid_argument> ([&] {
    optimizer.add_rule ("(rule duplicate (+ ?x 0) (+ ?x ?x))",
                        [] (const TermRule &) { return true; });
  });
  reject<std::runtime_error> ([&] { optimizer.optimize (source, 0); });
  reject<std::invalid_argument> ([&] { optimizer.optimize (parse_sexpr ("(f \"x\")")); });

  const auto imperative = hoare ();
  HoareState store{ { { "keep", 99 } } };
  auto factorial = imperative.compile_statement (R"(
    (triple (= n 5)
      (begin (set result 1)
        (while (< 0 n) (< -1 n)
          (begin (set result (* result n)) (set n (- n 1)))))
      (= result 120))
  )");
  store.variables["n"] = 5;
  ExecutionContract<HoareState> contract;
  PureExecutable<HoareState> predicate_language;
  predicate_language.on_atom ([] (auto &builder, const SExpr &syntax) {
    if (syntax.quoted || syntax.text != "positive")
      throw std::invalid_argument ("unknown predicate");
    return builder.expression ([] (auto &frame) {
      return frame.state ().get ().variables.at ("n") > 0;
    });
  });
  const auto positive = predicate_language.compile_expression ("positive");
  int predicates_executed = 0;
  contract.require (ContractPhase::precondition, "positive input",
                    [&] (const HoareState &old, const HoareState &) {
                      ++predicates_executed;
                      auto read_only = std::cref (old);
                      return positive.run (read_only);
                    });
  contract.require (ContractPhase::postcondition, "n exhausted",
                    [] (const HoareState &old, const HoareState &now) {
                      return old.variables.at ("n") == 5 && now.variables.at ("n") == 0;
                    });
  contract.require (ContractPhase::frame_condition, "keep unchanged",
                    [] (const HoareState &old, const HoareState &now) {
                      return old.variables.at ("keep") == now.variables.at ("keep");
                    });
  check (contract.obligations ().size () == 3 && predicates_executed == 0);
  auto capture = [] (const HoareState &state) { return state; };
  contract.run (factorial, store, capture, Hoare::Limits{});
  check (store.variables.at ("result") == 120 && predicates_executed == 1);
  reject<std::runtime_error> ([&] {
    contract.run (factorial, store, capture, Hoare::Limits{});
  });
  reject<std::runtime_error> ([&] {
    imperative.compile_statement ("(while 1 1 (begin))").run (store, { .steps = 15 });
  });
  reject<std::runtime_error> ([&] {
    imperative.compile_statement ("(while 0 0 (begin))").run (store);
  });

  const auto functional = lisp ();
  LispState closure_store;
  closure_store.context_value = 42;
  check (std::get<std::int64_t> (functional.compile_expression (
      "(let host-context 9 (context-value))").run (closure_store)) == 42);
  const auto lexically_scoped = functional.compile_expression (R"(
    (let x 40 (let f (lambda (y) (+ x y)) (let x 99 (f 2))))
  )");
  check (std::get<std::int64_t> (lexically_scoped.run (closure_store)) == 42);
  check (!closure_store.scope);
  functional.compile_statement ("(define make (lambda (x) (lambda (y) (+ x y))))")
      .run (closure_store);
  functional.compile_statement ("(define plus40 (make 40))").run (closure_store);
  check (std::get<std::int64_t> (
      functional.compile_expression ("(if true (plus40 2) (/ 1 0))").run (closure_store))
      == 42);
  auto before_failure = closure_store.scope;
  reject<std::runtime_error> ([&] {
    functional.compile_expression ("(let x 9 unbound)").run (closure_store);
  });
  check (before_failure == closure_store.scope);
  reject<std::invalid_argument> ([&] {
    functional.compile_expression ("(lambda (a a) a)");
  });
  reject<std::runtime_error> ([&] {
    functional.compile_expression ("((lambda (x) (x x)) (lambda (x) (x x)))")
        .run (closure_store);
  });

  struct AbstractMachine
  {
    std::vector<int> stack;
    std::size_t pc = 0;
  } machine;
  const auto transition = [] (AbstractMachine &state) {
    switch (state.pc++)
      {
      case 0: state.stack.push_back (40); return MachineControl::next;
      case 1: state.stack.push_back (2); return MachineControl::next;
      case 2:
        {
          const auto rhs = state.stack.at (1);
          state.stack.pop_back ();
          state.stack.at (0) += rhs;
          return MachineControl::halt;
        }
      default: throw std::runtime_error ("invalid demo PC");
      }
  };
  check (run_machine (machine, transition, 3) == 3 && machine.stack.at (0) == 42);
  reject<std::runtime_error> ([&] {
    run_machine (machine, [] (auto &) { return MachineControl::next; }, 5);
  });

  const auto evaluate = [&] (std::string_view source) {
    return std::to_string (numbers.compile_expanded_expression (macro_text, source).run (empty));
  };
  auto cli_eval = make_evaluation_adapter<repl::Evaluation> (evaluate);
  repl::Evaluation cli_result;
  cli::Command command;
  command.name = "arithmetic";
  cli::Positional source_argument;
  source_argument.name = "source";
  command.positionals.push_back (std::move (source_argument));
  command.handler = [&] (const cli::ParseResult &parsed) {
    cli_result = cli_eval (parsed.positionals.at (0));
  };
  const cli::OptParser opts (std::move (command));
  check (static_cast<bool> (opts.dispatch ({ "(twice 21)" })));
  check (cli_result.success && cli_result.output == "42");
  check (static_cast<bool> (opts.dispatch ({ "(/ 1 0)" })));
  check (!cli_result.success);
  repl::Session session ({}, make_evaluation_adapter<repl::Evaluation> (evaluate));
  check (session.editor ().insert ("(twice 21)"));
  const auto submitted = session.submit ();
  check (submitted.status == repl::SubmitStatus::evaluated && submitted.result.success
         && submitted.result.output == "42");
  auto untrusted_output = make_evaluation_adapter<repl::Evaluation> ([] (std::string_view) {
    return "\x1b[31m";
  });
  check (untrusted_output ("").output.find ('\x1b') == std::string::npos);
}
