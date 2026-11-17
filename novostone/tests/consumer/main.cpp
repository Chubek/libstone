#include <novostone/CostCC/CostCC.hpp>
#include <novostone/SpeculativeIR/SpeculativeIR.hpp>
#include <novostone/ActionSemantics/ActionSemantics.hpp>

int main() {
  using namespace novostone;
  auto language = action::parse("language Installed { start E; syntax E = N(); rule N = give 42; }");
  if (action::generate(language, action::Target::C).find("int main(void)") == std::string::npos) return 4;
  auto module = datalog::parse(".decl E(integer). E(42). .decl R(integer) derived. R(X) :- E(X).");
  ccdb::Database facts(module.program);
  auto transaction = facts.begin(); transaction.insert(module.facts);
  if (transaction.commit().query("R").rows != datalog::Relation{{42LL}}) return 1;

  ccdb::Database database(ir::program());
  ir::Builder builder(database, 1);
  auto block = builder.block(), argument = builder.emit(block, ir::Op::Param);
  auto zero = builder.emit(block, ir::Op::Const);
  auto value = builder.emit(block, ir::Op::Add, {argument, zero});
  builder.emit(block, ir::Op::Return, {value});
  auto source = builder.finish();
  auto optimized = query::optimize(source);
  cost::Ranker ranker;
  auto decision = ranker.choose({query::baseline(source), optimized}, {"installed", "i32", "none", "test", 1});
  if (decision.selected != 1 || ir::evaluate(ir::load(optimized.result()), {42}).value != 42) return 2;

  auto assumptions = std::make_shared<speculative::Assumptions>();
  auto id = assumptions->assume("usually 42");
  auto version = speculative::Version::specialize(source, assumptions, {{argument, 42, id}});
  assumptions->invalidate(id);
  auto recovered = version.execute({7});
  return recovered.failed_guard && recovered.outcome.value == 7 ? 0 : 3;
}
