#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "LibDSL.hpp"

namespace
{
using namespace dsl;
using namespace dsl::synth;

template <class Error = std::invalid_argument, class Test> void rejects (Test test)
{
  try
    {
      test ();
      assert (false);
    }
  catch (const Error &)
    {
    }
}

struct Host : DSL<Host, LanguageSynthesizer> {};
}

int main ()
{
  auto spec = Specification::from_text (R"(
    (language Example
      (form lambda 2)
      (form begin 1 *)
      (macro (twice $x) (+ $x $x))
      (macro (same $x $x) (quote same))
      (spec-macro (make-form $name) (form $name 0))
      (expand-spec (make-form empty))
      (state Store (record (count integer) (flags (list boolean))))
      (expression sum (+ left right))
      (statement print (emit value))
      (attribute type (infer value))
      (metaproc infer (lambda (x) x))
      (diagnostic BadType "a different type is required")
      (grammar (start Expr) (rule Expr ("x") ())))
  )");
  const SyntaxOperand left ("left"), right ("right");
  auto native = SpecBuilder ("Example")
      .form<"lambda", 2> ()
      .form ({ "begin", 1, std::nullopt })
      .macro (TermRule{ "twice", parse_term ("(twice ?x)"), parse_term ("(+ ?x ?x)") })
      .macro (parse_sexpr ("(same $x $x)"), parse_sexpr ("(quote same)"))
      .macro (parse_sexpr ("(make-form $name)"), parse_sexpr ("(form $name 0)"),
              MacroPhase::specification)
      .form ("empty", 0)
      .declare (DeclarationKind::state, "Store",
                parse_sexpr ("(record (count integer) (flags (list boolean)))"))
      .expression ("sum", left + right)
      .declare (DeclarationKind::statement, "print", ASTNode ("emit", std::string ("value")))
      .declare (DeclarationKind::attribute, "type", parse_sexpr ("(infer value)"))
      .declare (DeclarationKind::metaproc, "infer", parse_sexpr ("(lambda (x) x)"))
      .declare (DeclarationKind::diagnostic, "BadType",
                SExpr::atom ("a different type is required", true))
      .grammar (make_cfg<"Expr"> (CFGProduction<"Expr", CFGTerminal<"x">>{},
                                 CFGProduction<"Expr">{}))
      .build ();
  assert (spec.to_sexpr () == native.to_sexpr ());
  assert (Specification::from_text (spec.to_sexpr ().dump ()).to_sexpr () ==
          spec.to_sexpr ());
  assert (spec.grammar ()->parse ({ "x" }).ok ());
  assert (spec.grammar ()->parse ({}).ok ());
  assert (!spec.grammar ()->parse ({ "y" }).ok ());

  auto language = Host::synthesize (spec.to_sexpr ());
  language.validate_form (parse_sexpr ("(lambda (x) x)"));
  language.validate_form (parse_sexpr ("(empty)"));
  language.validate_form (parse_sexpr ("(begin x y z)"));
  rejects ([&] { language.validate_form (parse_sexpr ("(begin)")); });
  assert (language.expand ("(twice (twice a))").dump () == "(+ (+ a a) (+ a a))");
  assert (language.expand ("(same (x) (x))").dump () == "(quote same)");
  assert (language.expand ("(same x \"x\")").dump () == "(same x \"x\")");
  assert (language.expand ("(quote (twice a))").dump () == "(quote (twice a))");
  assert (language.expand ("(make-form z)").dump () == "(make-form z)");
  auto provenance = language.expand_report (parse_sexpr ("(begin (twice a))"));
  assert ((provenance.provenance[0].path == std::vector<std::size_t>{ 1 }));
  assert (provenance.provenance[0].macro_index == 1);

  spec.validate_state ("Store", parse_sexpr ("(record (flags (true false)) (count -8))"));
  rejects ([&] { spec.validate_state ("Store", parse_sexpr ("(record (count -) (flags (true)))")); });
  rejects ([&] { spec.validate_state ("Store", parse_sexpr ("(record (count 0) (count 1))")); });
  struct Counter { int value; };
  Counter counter{ 0 };
  NativeState<Counter> state (spec);
  state.operation ("advance", [] (Counter &current, std::span<const SExpr>)
    { return SExpr::atom (std::to_string (++current.value)); });
  assert (state.invoke (counter, "advance", {}).text == "1");
  rejects ([&] { state.invoke (counter, "missing", {}); });

  MetaEvaluator eval;
  eval.primitive ("echo", [] (std::span<const MetaValue> args)
    {
      if (args.size () != 1)
        throw std::invalid_argument ("echo requires one argument");
      return args[0];
    });
  assert (eval.evaluate ("(((lambda (x) (lambda (y) (list x y))) "
                         "(quote one)) (quote two))").as_syntax ().dump () ==
          "(one two)");
  assert (eval.evaluate ("(let (a (b _)) (quote (1 (2 3))) "
                         "(if (equal? b 2) (echo a) never))").as_syntax ().text == "1");
  assert (eval.evaluate ("(let x 1 (let f (lambda () x) (let x 2 (f))))")
            .as_syntax ().text == "1");
  rejects ([&] { eval.evaluate ("(let (a a) (quote (1 2)) a)"); });
  rejects ([&] { eval.evaluate ("(if \"true\" 1 2)"); });
  rejects ([&] { eval.evaluate ("(unregistered)"); });
  rejects<std::length_error> ([&] {
    eval.evaluate ("((lambda (f) (f f)) (lambda (f) (f f)))", { 8, 10000 });
  });
  rejects<std::length_error> ([&] { eval.evaluate ("(echo 1)", { 128, 10 }); });

  auto hygienic = Host::synthesize (
      "(language Hygienic"
      "  (macro (bind $x) (let (fresh temp) 1 (list (fresh temp) $x)))"
      "  (macro (loop $x) (loop $x)))");
  auto hygienic_syntax = hygienic.expand ("(let __dsl_macro_1 5 (bind __dsl_macro_1))");
  assert (eval.evaluate (hygienic_syntax).as_syntax ().dump () == "(1 5)");
  assert (eval.evaluate (hygienic.expand ("(list (bind 7) (bind 8))"))
            .as_syntax ().dump () == "((1 7) (1 8))");
  rejects<std::length_error> ([&] { hygienic.expand ("(loop x)", { 128, 3, 10000 }); });
  rejects<std::length_error> ([&] { hygienic.expand ("(bind x)", { 1, 3, 10000 }); });

  rejects ([] { Specification::from_text ("(language X (state S (list bad)))"); });
  rejects ([] { Specification::from_text ("(language X (state S (record (x atom) (x atom))))"); });
  rejects ([] { Specification::from_text ("(language X (diagnostic D message))"); });
  rejects ([] { Specification::from_text ("(language X (form a 2 1))"); });
  rejects ([] { Specification::from_text ("(language X (form a 1) (form a 2))"); });
  rejects ([] { Specification::from_text ("(language X (macro (f $x) $y))"); });
  rejects ([] { Specification::from_text ("(language X (macro (f $x) (fresh)))"); });
  rejects ([] { Specification::from_text ("(language X (grammar (start S) (start R)))"); });
  rejects ([] { Specification::from_text ("(language X (do-not-know S))"); });
  rejects ([] { Specification::from_sexpr ({ true, "language", { SExpr::atom ("x") }, false }); });
  rejects<std::out_of_range> ([] {
    Specification::from_text ("(language X (form a 999999999999999999999999999999))");
  });
}
