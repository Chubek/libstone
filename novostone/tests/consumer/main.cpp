#include <novostone/CostCC/CostCC.hpp>
#include <novostone/PCC/Native.hpp>
#include <novostone/PCC/SMTProof.hpp>
#include <novostone/SpeculativeIR/SpeculativeIR.hpp>
#include <novostone/ActionSemantics/ActionSemantics.hpp>

#ifdef TEST_PCC_PACKAGE
#include <novostone/PCC/Package.hpp>
#include <sstream>
#endif

int main() {
  using namespace novostone;
  auto language = action::parse("language Installed { start E; syntax E = N(); rule N = give 42; }");
  if (action::generate(language, action::Target::C).find("int main(") == std::string::npos) return 4;
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
  if (!recovered.failed_guard || recovered.outcome.value != 7) return 3;

  auto proof_input = ir::parse(
      "novostone 1 0 1\n"
      "block 1\n"
      "instr 2 const 40 0 0\n"
      "instr 3 const 2 0 0\n"
      "instr 4 add 0 2 2 3 0\n"
      "instr 5 return 0 1 4 0\n");
  auto artifact = pcc::build(proof_input, pcc::RewriteConstraints{});
  auto proof = pcc::emit_smt_proof(artifact);
  if (artifact.transformations().size() != 1 ||
      proof.obligations.size() != 1) return 5;
  auto result = pcc::run_smtlib2(proof.obligations.front());
  if (pcc::satie_smtlib2_available() &&
      result.status != pcc::ObligationStatus::UnsatProved) return 6;
  if (!pcc::satie_smtlib2_available() &&
      result.status != pcc::ObligationStatus::Unknown) return 7;
  const auto native = pcc::lower_native(artifact.transformed());
  if (!native.program) return 8;
  const auto checked = pcc::verify_native(artifact.transformed(), *native.program);
  if (!checked.binding_checked || checked.assembly.empty()) return 9;
  if (checked.status != (pcc::satie_smtlib2_available()
                            ? pcc::NativeStatus::Proved
                            : pcc::NativeStatus::Unknown)) return 10;
#ifdef TEST_PCC_PACKAGE
  std::istringstream invalid;
  try {
    (void)pcc::read_package(invalid);
    return 11;
  } catch (const pcc::PackageError&) {
    // The installed package target performs the same bounded framing checks.
  }
#endif
  return 0;
}
