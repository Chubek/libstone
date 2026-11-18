#include "novostone/PCC/SMTProof.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace novostone;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template<class Action>
void rejects(Action action, const std::string& message) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected, message);
}

ir::Function folding_function(std::uint32_t right = 2) {
  ir::Function function;
  function.entry = 1;
  ir::Block block;
  block.id = 1;
  block.instructions = {
      {2, ir::Op::Const, std::numeric_limits<std::uint32_t>::max(), {}, {}},
      {3, ir::Op::Const, right, {}, {}},
      {4, ir::Op::Add, 0, {2, 3}, {}},
      {5, ir::Op::Return, 0, {4}, {}},
  };
  function.blocks.emplace(block.id, std::move(block));
  ir::verify(function);
  return function;
}

ir::Function cse_function() {
  ir::Function function;
  function.arity = 2;
  function.entry = 1;
  ir::Block block;
  block.id = 1;
  block.instructions = {
      {2, ir::Op::Param, 0, {}, {}},
      {3, ir::Op::Param, 1, {}, {}},
      {4, ir::Op::Add, 0, {2, 3}, {}},
      {5, ir::Op::Add, 0, {3, 2}, {}},
      {6, ir::Op::Return, 0, {5}, {}},
  };
  function.blocks.emplace(block.id, std::move(block));
  ir::verify(function);
  return function;
}

ir::Function algebraic_function() {
  ir::Function function;
  function.arity = 1;
  function.entry = 1;
  ir::Block block;
  block.id = 1;
  block.instructions = {
      {2, ir::Op::Param, 0, {}, {}},
      {3, ir::Op::Const, 0, {}, {}},
      {4, ir::Op::Add, 0, {2, 3}, {}},
      {5, ir::Op::Return, 0, {4}, {}},
  };
  function.blocks.emplace(block.id, std::move(block));
  ir::verify(function);
  return function;
}

ir::Function uniform_phi_function() {
  ir::Function function;
  function.arity = 1;
  function.entry = 10;
  function.blocks.emplace(
      10, ir::Block{10,
                    {{2, ir::Op::Param, 0, {}, {}},
                     {3, ir::Op::Const, 7, {}, {}},
                     {4, ir::Op::Branch, 0, {2}, {20, 30}}}});
  function.blocks.emplace(
      20, ir::Block{20, {{5, ir::Op::Jump, 0, {}, {40}}}});
  function.blocks.emplace(
      30, ir::Block{30, {{6, ir::Op::Jump, 0, {}, {40}}}});
  function.blocks.emplace(
      40, ir::Block{40,
                    {{7, ir::Op::Phi, 0, {3, 3}, {20, 30}},
                     {8, ir::Op::Return, 0, {7}, {}}}});
  ir::verify(function);
  return function;
}

ir::Function cfg_fixture(bool equal_targets = false) {
  using ir::Op;
  ir::Function f;
  f.arity = 1;
  f.entry = 10;
  f.blocks = {
      {10, {10, {{1, Op::Param, 0, {}, {}},
                 {2, Op::Const, 0x80000000, {}, {}},
                 {3, Op::Jump, 0, {}, {20}}}}},
      {20, {20, {{4, Op::Phi, 0, {1, 5}, {10, 30}},
                 {5, Op::Phi, 0, {2, 4}, {10, 30}},
                 {6, Op::Branch, 0, {1}, {30, 40}}}}},
      {30, {30, {{7, Op::Branch, 0, {equal_targets ? 1 : 2},
                                 {40, equal_targets ? 40 : 20}}}}},
      {40, {40, {{8, Op::Phi, 0, {4, 5}, {20, 30}},
                 {9, Op::Return, 0, {8}, {}}}}}
  };
  if (equal_targets) {
    for (auto& i : f.blocks.at(20).instructions) {
      if (i.op != Op::Phi) break;
      i.operands.pop_back();
      i.targets.pop_back();
    }
  }
  ir::verify(f);
  return f;
}

ir::Function memory_fixture(bool ambiguous = false) {
  using ir::Op;
  ir::Function f;
  f.arity = 3;
  f.entry = 10;
  f.blocks.emplace(10, ir::Block{10, {
      {1, Op::Param, 0, {}, {}}, {2, Op::Param, 1, {}, {}},
      {3, Op::Param, 2, {}, {}}, {4, Op::Const, 0, {}, {}},
      {5, Op::Const, 1, {}, {}}, {6, Op::Store, 0, {4, 1}, {}},
      {7, Op::Store, 0, {ambiguous ? 3 : 5, 2}, {}},
      {8, Op::Load, 0, {4}, {}}, {9, Op::Return, 0, {8}, {}}
  }});
  ir::verify(f);
  return f;
}

ir::Function dead_cycle_fixture() {
  using ir::Op;
  ir::Function f;
  f.arity = 1;
  f.entry = 10;
  f.blocks = {
    {10, {10, {{1, Op::Param, 0, {}, {}}, {2, Op::Const, 7, {}, {}},
               {3, Op::Jump, 0, {}, {20}}}}},
    {20, {20, {{4, Op::Phi, 0, {2, 5}, {10, 20}},
               {5, Op::Phi, 0, {2, 4}, {10, 20}},
               {6, Op::Branch, 0, {1}, {20, 30}}}}},
    {30, {30, {{7, Op::Return, 0, {1}, {}}}}}
  };
  ir::verify(f);
  return f;
}

ir::Function conditional_fixture() {
  using ir::Op;
  ir::Function f;
  f.arity = 1;
  f.entry = 10;
  f.blocks = {
    {10, {10, {{1, Op::Param, 0, {}, {}}, {2, Op::Branch, 0, {1}, {20, 30}}}}},
    {20, {20, {{3, Op::Const, 7, {}, {}}, {4, Op::Jump, 0, {}, {40}}}}},
    {30, {30, {{5, Op::Const, 7, {}, {}}, {6, Op::Jump, 0, {}, {40}}}}},
    {40, {40, {{7, Op::Phi, 0, {3, 5}, {20, 30}},
               {8, Op::Phi, 0, {1, 5}, {20, 30}},
               {9, Op::Branch, 0, {7}, {50, 60}}}}},
    {50, {50, {{11, Op::Return, 0, {8}, {}}}}},
    {60, {60, {{12, Op::Load, 0, {1}, {}}, {13, Op::Return, 0, {12}, {}}}}}
  };
  ir::verify(f);
  return f;
}

ir::Function conditional_loop_fixture(bool different_seed = false) {
  using ir::Op;
  ir::Function f;
  f.arity = 1;
  f.entry = 20;
  f.blocks = {
    {20, {20, {{1, Op::Param, 0, {}, {}}, {2, Op::Const, 7, {}, {}},
               {3, Op::Const, 0, {}, {}}, {4, Op::Jump, 0, {}, {30}}}}},
    {30, {30, {{5, Op::Phi, 0, {2, 6}, {20, 30}},
               {6, Op::Phi, 0, {different_seed ? 13 : 2, 5}, {20, 30}},
               {7, Op::Phi, 0, {3, 8}, {20, 30}},
               {8, Op::Add, 0, {7, 2}, {}},
               {9, Op::Ult, 0, {7, 1}, {}},
               {10, Op::Branch, 0, {9}, {30, 40}}}}},
    {40, {40, {{11, Op::Add, 0, {5, 7}, {}}, {12, Op::Return, 0, {11}, {}}}}}
  };
  if (different_seed)
    f.blocks.at(20).instructions.insert(f.blocks.at(20).instructions.end() - 1,
                                        {13, Op::Const, 8, {}, {}});
  ir::verify(f);
  return f;
}

pcc::CallingConvention leaf_convention(bool with_callee_saved = false) {
  pcc::CallingConvention convention;
  convention.register_count = 4;
  convention.argument_registers = {0, 1};
  convention.return_register = 0;
  convention.caller_saved = with_callee_saved
                                ? std::set<pcc::PhysicalRegister>{0, 1}
                                : std::set<pcc::PhysicalRegister>{0, 1, 2};
  if (with_callee_saved) convention.callee_saved = {2};
  convention.reserved = {3};
  pcc::validate_calling_convention(convention);
  return convention;
}

void expect_unsat_or_unavailable(const pcc::SMTConduitResult& result,
                                 const std::string& subject) {
  if (pcc::satie_smtlib2_available()) {
    require(result.syntax == pcc::SMTSyntaxStatus::Valid,
            subject + " must be valid SMT-LIB2: " + result.diagnostic);
    require(result.status == pcc::ObligationStatus::UnsatProved,
            subject + " must be proved UNSAT: " + result.diagnostic);
  } else {
    require(result.status == pcc::ObligationStatus::Unknown,
            subject + " must remain Unknown without Satie");
  }
}

pcc::Artifact prove_rewrites(const ir::Function& function,
                             const std::set<std::string>& rules,
                             std::size_t max_rewrites = 1000) {
  auto policy = pcc::RewriteConstraints::all_query_rewrites();
  policy.limits.max_rewrites = max_rewrites;
  const auto artifact = pcc::build(function, policy);
  const auto proof = pcc::emit_smt_proof(artifact);
  const auto again = pcc::emit_smt_proof(artifact);
  require(proof.obligations.size() == artifact.transformations().size(),
          "must prove every accepted step");
  auto remaining = rules;
  for (std::size_t i = 0; i < proof.obligations.size(); ++i) {
    const auto& obligation = proof.obligations[i];
    require(obligation.ordinal == i && obligation.revision == i + 1 &&
            obligation.rule == artifact.transformations().at(i).rule &&
            obligation.smtlib2 == again.obligations[i].smtlib2,
            "proof must preserve replay identity and be deterministic");
    remaining.erase(obligation.rule);
    expect_unsat_or_unavailable(pcc::run_smtlib2(obligation), obligation.rule);
  }
  require(remaining.empty(), "fixture did not execute the expected rule");
  return artifact;
}

void remaining_rewrite_tests() {
  using ir::Op;
  const auto backedge = cfg_fixture();
  const auto folded = prove_rewrites(backedge, {"fold_branch"}, 1);
  require(folded.transformed().blocks.size() == 4 &&
          folded.transformed().blocks.at(20).instructions.at(0).targets ==
              std::vector<ir::Id>{10} &&
          folded.transformed().blocks.at(20).instructions.at(1).operands ==
              std::vector<ir::Id>{2},
          "fold must repair both phis when the former backedge block survives");
  for (const auto arg : {0u, 1u})
    require(ir::equivalent(ir::evaluate(backedge, {arg}),
                           ir::evaluate(folded.transformed(), {arg})),
            "backedge fold must preserve parallel-phi results");

  auto alternate = uniform_phi_function();
  alternate.blocks.at(10).instructions.at(0) = {2, Op::Const, 0, {}, {}};
  alternate.blocks.at(20).instructions.insert(
      alternate.blocks.at(20).instructions.begin(), {9, Op::Load, 0, {3}, {}});
  const auto trimmed = prove_rewrites(alternate, {"fold_branch"}, 1);
  require(!trimmed.transformed().blocks.contains(20) &&
          ir::equivalent(ir::evaluate(alternate, {0}),
                         ir::evaluate(trimmed.transformed(), {0})),
          "zero condition removes unreachable trapping arm");
  const auto duplicate = cfg_fixture(true);
  // Phis would otherwise be reducible after this exact next rewrite.
  const auto same_target = prove_rewrites(duplicate, {"fold_branch"}, 1);
  require(ir::equivalent(ir::evaluate(duplicate, {2}),
                         ir::evaluate(same_target.transformed(), {2})),
          "duplicate branch targets fold for a non-constant condition");

  const auto memory = memory_fixture();
  const auto forwarded = prove_rewrites(memory, {"forward_load"}, 1);
  for (const auto& initial : {std::vector<std::uint32_t>{}, {0}, {0, 0}}) {
    const auto a = ir::evaluate(memory, {91, 42, 0}, initial);
    const auto b = ir::evaluate(forwarded.transformed(), {91, 42, 0}, initial);
    require(ir::equivalent(a, b), "forwarding must preserve earlier traps and write trace");
  }
  auto prior_load = memory;
  auto& loads = prior_load.blocks.at(10).instructions;
  loads[5] = {6, Op::Load, 0, {3}, {}};
  loads[6] = {7, Op::Load, 0, {5}, {}};
  loads[7].operands = {3};
  const auto reused = prove_rewrites(prior_load, {"forward_load"}, 1);
  for (const auto& initial : {std::vector<std::uint32_t>{37}, {37, 44}})
    require(ir::equivalent(ir::evaluate(prior_load, {0, 0, 0}, initial),
                           ir::evaluate(reused.transformed(), {0, 0, 0}, initial)),
            "load-to-load forwarding still executes an intervening trapping load");

  const auto cycle = dead_cycle_fixture();
  const auto eliminated = prove_rewrites(cycle, {"dead_pure_cycle"}, 1);
  require(eliminated.transformed().blocks.at(20).instructions.size() == 1 &&
          ir::evaluate(eliminated.transformed(), {0}).value == 0 &&
          ir::evaluate(eliminated.transformed(), {1}, {}, 30).trap == "step budget",
          "unrooted mutually recursive phis may disappear but loop control must remain");
  (void)prove_rewrites(folding_function(), {"fold_add", "dead_pure"});

  const auto conditional = conditional_fixture();
  const auto propagated = prove_rewrites(conditional,
      {"conditional_constant", "fold_branch", "dead_pure"});
  for (const auto arg : {0u, 1u, 0xffffffffu})
    require(ir::equivalent(ir::evaluate(conditional, {arg}),
                           ir::evaluate(propagated.transformed(), {arg})),
            "SCCP constant from distinct SSA inputs must preserve results");
  const auto loop = conditional_loop_fixture();
  const auto induction = prove_rewrites(loop, {"conditional_constant"}, 1);
  const auto& phis = induction.transformed().blocks.at(30).instructions;
  require(phis[0].id == 6 && phis[1].id == 7 &&
          phis[2].id == 5 && phis[2].op == Op::Const,
          "constant phi must move after remaining parallel phis");
  for (const auto arg : {0u, 7u, 14u})
    require(ir::equivalent(ir::evaluate(loop, {arg}),
                           ir::evaluate(induction.transformed(), {arg})),
            "SCCP inductive invariant must hold across loop iterations");
}

void rewrite_tests() {
  const auto source = folding_function();
  pcc::RewriteConstraints constraints;
  const auto first = pcc::build(source, constraints);
  const auto second = pcc::build(source, constraints);

  require(first.canonical_core() == second.canonical_core(),
          "PCC artifact must be deterministic");
  require(first.transformations().size() == 1,
          "wrapping addition should have one accepted fold");
  require(first.transformations().front().kind ==
              pcc::TransformKind::ConstantFolding,
          "fold must be classified");
  const auto result = ir::evaluate(first.transformed());
  require(result.value == 1, "constant fold must preserve modulo-i32 wrap");
  require(first.selection().stop == pcc::StopCause::ConstraintBoundary,
           "default policy should stop before disabled DCE");
  require(!first.analysis().values.empty(), "source analysis must be shipped");

  constraints.disallow(pcc::TransformKind::ConstantFolding);
  const auto blocked = pcc::build(source, constraints);
  require(blocked.transformations().empty(),
          "disabled fold must not be applied");
  require(ir::dump(blocked.source()) == ir::dump(blocked.transformed()),
          "blocked artifact must preserve source IR");
  require(blocked.selection().blocked.has_value(),
          "constraint boundary must identify the blocked rewrite");

  const auto cse = pcc::build(cse_function(), pcc::RewriteConstraints{});
  require(cse.transformations().size() == 1 &&
              cse.transformations().front().kind ==
                  pcc::TransformKind::LocalCSE,
          "local CSE should be selected under the default policy");
  require(ir::evaluate(cse.transformed(), {17, 25}).value == 42,
          "CSE result must execute correctly");

  pcc::RewriteConstraints zero_budget;
  zero_budget.limits.max_rewrites = 0;
  const auto unchanged = pcc::build(source, zero_budget);
  require(unchanged.transformations().empty() &&
              unchanged.selection().stop == pcc::StopCause::ResourceLimit,
          "zero rewrite budget must return an unchanged bounded artifact");
}

void smt_tests() {
  auto fold = pcc::build(folding_function(), pcc::RewriteConstraints{});
  auto proof = pcc::emit_smt_proof(fold);
  require(proof.obligations.size() == fold.transformations().size(),
          "every accepted rewrite must have one obligation");
  require(proof.obligations.front().encoding == "i32-bool-bitblast-v1",
          "constant fold must use exact i32 bit-blasting");
  require(proof.obligations.front().smtlib2.find("(check-sat)") !=
              std::string::npos,
          "proof obligation must be executable SMT-LIB2");
  pcc::discharge_smt(proof);
  expect_unsat_or_unavailable(
      pcc::run_smtlib2(proof.obligations.front()),
      "constant-fold obligation");
  if (pcc::satie_smtlib2_available())
    require(proof.all_proved(), "all generated fold obligations must prove");

  const auto cse_artifact =
      pcc::build(cse_function(), pcc::RewriteConstraints{});
  auto cse = pcc::emit_smt_proof(cse_artifact);
  require(cse.obligations.size() == 1,
          "CSE must produce one proof obligation");
  require(cse.obligations.front().smtlib2.find("g_") != std::string::npos,
          "CSE SMT must retain the eliminated expression's bit circuit");
  require(cse.obligations.front().smtlib2.find("g_000300") !=
              std::string::npos,
          "CSE SMT must independently encode both matching expressions");
  expect_unsat_or_unavailable(
      pcc::run_smtlib2(cse.obligations.front()), "CSE obligation");

  pcc::RewriteConstraints local_constraints;
  local_constraints.allow(pcc::TransformKind::AlgebraicSimplification);
  local_constraints.allow(pcc::TransformKind::CopyPropagation);
  const auto local_artifact =
      pcc::build(algebraic_function(), local_constraints);
  require(local_artifact.transformations().size() == 2 &&
              local_artifact.transformations()[0].kind ==
                  pcc::TransformKind::AlgebraicSimplification &&
              local_artifact.transformations()[1].kind ==
                  pcc::TransformKind::CopyPropagation,
          "algebraic replacement and copy propagation must be recorded");
  auto local = pcc::emit_smt_proof(local_artifact);
  require(local.obligations.size() == 2,
          "each local replacement must have an obligation");
  for (const auto& obligation : local.obligations)
    expect_unsat_or_unavailable(pcc::run_smtlib2(obligation),
                                "local replacement obligation");

  pcc::RewriteConstraints phi_constraints;
  phi_constraints.allow(pcc::TransformKind::PhiSimplification);
  const auto phi_artifact =
      pcc::build(uniform_phi_function(), phi_constraints);
  require(phi_artifact.transformations().size() == 1 &&
              phi_artifact.transformations().front().kind ==
                  pcc::TransformKind::PhiSimplification,
          "uniform phi elimination must be recorded");
  const auto phi = pcc::emit_smt_proof(phi_artifact);
  require(phi.obligations.size() == 1,
          "uniform phi elimination must have one obligation");
  expect_unsat_or_unavailable(pcc::run_smtlib2(phi.obligations.front()),
                              "uniform-phi obligation");

  require(pcc::parse_smt_frontend("smtlib2") ==
              pcc::SMTFrontend::SMTLIB2,
          "selected frontend spelling");
  rejects([] { (void)pcc::parse_smt_frontend("cnf"); },
          "unselected frontend must be rejected");
  const auto safe = pcc::smt_identifier("x)\n(assert false);");
  require(safe.starts_with("n_") &&
              safe.find_first_not_of("n_0123456789abcdef") ==
                  std::string::npos,
          "SMT identifiers must not permit source injection");

  const auto sat = pcc::run_smtlib2(
      "(set-logic QF_UF)\n(declare-const witness Bool)\n"
      "(assert witness)\n(check-sat)\n");
  if (pcc::satie_smtlib2_available())
    require(sat.status == pcc::ObligationStatus::SatCounterexample,
            "Satie must distinguish SAT counterexamples from proofs");
  else
    require(sat.status == pcc::ObligationStatus::Unknown,
            "absent conduit must remain Unknown");

  remaining_rewrite_tests();
}

void regalloc_tests() {
  const auto function = cse_function();
  const auto convention = leaf_convention();
  const auto evidence = pcc::allocate_registers(function, convention);
  require(evidence.status == pcc::EvidenceStatus::ObligationReady,
          "caller-saved leaf allocation should be obligation-ready");
  require(evidence.structurally_valid &&
              evidence.has_complete_allocation(),
          "allocator must produce a complete valid coloring");
  require(!evidence.native_calling_convention_proven,
          "abstract evidence must not claim native ABI proof");
  require(evidence.preservation_obligations.empty(),
          "caller-saved allocation needs no callee preservation premise");
  expect_unsat_or_unavailable(pcc::run_smtlib2(evidence.smtlib2),
                              "register-allocation obligation");

  pcc::RegisterAssignment invalid;
  for (const auto value : evidence.interference.values) invalid[value] = 0;
  const auto rejected = pcc::validate_register_allocation(
      function, convention, invalid);
  require(rejected.status == pcc::EvidenceStatus::Rejected,
          "interfering values in one register must be rejected");
  if (pcc::satie_smtlib2_available())
    require(pcc::run_smtlib2(rejected.smtlib2).status ==
                pcc::ObligationStatus::SatCounterexample,
            "invalid coloring must have a SAT violation witness");

  pcc::AllocationConstraints use_callee;
  use_callee.fixed_registers.emplace(4, 2);
  const auto unresolved = pcc::allocate_registers(
      function, leaf_convention(true), use_callee);
  require(unresolved.status == pcc::EvidenceStatus::Unknown &&
              !unresolved.preservation_obligations.empty(),
          "callee-saved use must remain Unknown without native preservation");
  if (pcc::satie_smtlib2_available())
    require(pcc::run_smtlib2(unresolved.smtlib2).status ==
                pcc::ObligationStatus::SatCounterexample,
            "unresolved preservation predicate must admit a countermodel");

  auto too_small = leaf_convention();
  too_small.register_count = 2;
  too_small.caller_saved = {0, 1};
  too_small.callee_saved.clear();
  too_small.reserved.clear();
  pcc::validate_calling_convention(too_small);
  const auto impossible = pcc::allocate_registers(function, too_small);
  require(impossible.status == pcc::EvidenceStatus::Rejected,
          "register-only allocation must reject an impossible coloring");
}

void negative_tests() {
  auto invalid_convention = leaf_convention();
  invalid_convention.reserved.insert(2);
  rejects([&] { pcc::validate_calling_convention(invalid_convention); },
          "overlapping register classes");

  pcc::AllocationOptions no_search;
  no_search.search_node_limit = 0;
  rejects([&] {
    (void)pcc::allocate_registers(cse_function(), leaf_convention(), {},
                                  no_search);
  }, "zero search budget");

  pcc::RewriteConstraints excessive;
  excessive.limits.max_instructions =
      std::numeric_limits<std::size_t>::max();
  rejects([&] { (void)pcc::build(folding_function(), excessive); },
          "hard resource cap");
  rejects([] {
    pcc::RewriteConstraints constraints;
    constraints.allow(static_cast<pcc::TransformKind>(999));
  }, "unknown transformation kind");

  auto after = folding_function();
  after.blocks.at(1).instructions.at(2) =
      {4, ir::Op::Const, 7, {}, {}};
  const query::Rewrite lie{1, 4, "fold_add",
                           {"Instr", "Operand"}};
  rejects([&] {
    (void)pcc::emit_smt_obligation(folding_function(), after, lie);
  }, "mismatched fold result");

  const auto reject_edit = [](const ir::Function& before,
                              const ir::Function& after,
                              ir::Id instruction, std::string rule,
                              const std::string& why) {
    ir::verify(before);
    ir::verify(after);
    const query::Rewrite rewrite{1, instruction, std::move(rule), {}};
    rejects([&] { (void)pcc::emit_smt_obligation(before, after, rewrite); }, why);
  };
  auto valid_fold = after;
  valid_fold.blocks.at(1).instructions.at(2).immediate = 1;
  reject_edit(folding_function(), valid_fold, 4, "unrecognized", "no generic proof");

  auto one = pcc::RewriteConstraints::all_query_rewrites();
  one.limits.max_rewrites = 1;
  const auto cfg = cfg_fixture();
  const auto branch = pcc::build(cfg, one);
  auto bad_cfg = branch.transformed();
  bad_cfg.blocks.at(20).instructions.at(1).operands = {1};
  reject_edit(cfg, bad_cfg, 7, "fold_branch", "phi repair must preserve surviving values");
  bad_cfg = branch.transformed();
  bad_cfg.blocks.at(40).instructions.at(0).operands = {5, 4};
  reject_edit(cfg, bad_cfg, 7, "fold_branch", "unrelated retained phi inputs cannot be swapped");
  auto dynamic_cfg = cfg;
  dynamic_cfg.blocks.at(30).instructions.front().operands = {1};
  reject_edit(dynamic_cfg, branch.transformed(), 7, "fold_branch",
              "must not fold a branch with unequal successors and arbitrary condition");

  const auto alias = memory_fixture(true);
  auto bad_forward = alias;
  bad_forward.blocks.at(10).instructions.at(7) = {8, ir::Op::Copy, 0, {1}, {}};
  reject_edit(alias, bad_forward, 8, "forward_load", "unknown write alias invalidates witness");
  require(ir::evaluate(alias, {91, 42, 0}, {0}).value == 42 &&
          ir::evaluate(bad_forward, {91, 42, 0}, {0}).value == 91,
          "alias fixture must distinguish the proposed wrong edit");
  auto clobber = alias;
  clobber.blocks.at(10).instructions.at(6).operands[0] = 4;
  bad_forward.blocks.at(10).instructions.at(6).operands[0] = 4;
  reject_edit(clobber, bad_forward, 8, "forward_load", "witness must be the latest coincident access");
  const auto memory = memory_fixture();
  bad_forward = pcc::build(memory, one).transformed();
  bad_forward.blocks.at(10).instructions.erase(
      bad_forward.blocks.at(10).instructions.begin() + 5);
  reject_edit(memory, bad_forward, 8, "forward_load", "must preserve witness and write trace");
  auto cross_block = memory;
  auto& first_block = cross_block.blocks.at(10).instructions;
  cross_block.blocks.emplace(20, ir::Block{20, {first_block[7], first_block[8]}});
  first_block.erase(first_block.begin() + 7, first_block.end());
  first_block.push_back({11, ir::Op::Jump, 0, {}, {20}});
  bad_forward = cross_block;
  bad_forward.blocks.at(20).instructions.at(0) = {8, ir::Op::Copy, 0, {1}, {}};
  reject_edit(cross_block, bad_forward, 8, "forward_load",
              "unsupported cross-block memory proof must fail closed");

  const auto cycle = dead_cycle_fixture();
  auto bad_dce = pcc::build(cycle, one).transformed();
  bad_dce.blocks.at(20).instructions.back().targets = {30, 20};
  reject_edit(cycle, bad_dce, 2, "dead_pure_cycle", "DCE must preserve observable CFG");
  auto rooted = cycle;
  rooted.blocks.at(30).instructions.insert(rooted.blocks.at(30).instructions.begin(),
                                          {8, ir::Op::Store, 0, {1, 4}, {}});
  reject_edit(rooted, pcc::build(cycle, one).transformed(), 2, "dead_pure_cycle",
              "a cycle reaching a store is observable");
  auto trap = memory;
  trap.blocks.at(10).instructions.back().operands = {1};
  auto erased_load = trap;
  erased_load.blocks.at(10).instructions.erase(erased_load.blocks.at(10).instructions.begin() + 7);
  reject_edit(trap, erased_load, 8, "dead_pure", "unused trapping loads are roots");

  const auto conditional = conditional_fixture();
  auto bad_sccp = pcc::build(conditional, one).transformed();
  bad_sccp.blocks.at(40).instructions.at(1).immediate = 0;
  reject_edit(conditional, bad_sccp, 7, "conditional_constant", "wrong constant in verified phi edit");
  const auto loop = conditional_loop_fixture(true);
  auto& block = (bad_sccp = loop).blocks.at(30).instructions;
  block.erase(block.begin());
  block.insert(block.begin() + 2, {5, ir::Op::Const, 7, {}, {}});
  reject_edit(loop, bad_sccp, 5, "conditional_constant",
              "loop phis that alternate between distinct values are not constant");
  require(ir::evaluate(loop, {7}).value == 15 && ir::evaluate(bad_sccp, {7}).value == 14,
          "fixed-point adversary must actually disagree on the second visit");

  // Artifacts expose read-only fields, but demand exact replay even under a
  // consumer who corrupts an otherwise well-formed transformation record.
  auto corrupted = branch;
  auto& records = const_cast<std::vector<pcc::Transformation>&>(corrupted.transformations());
  records.front().kind = pcc::TransformKind::DeadCodeElimination;
  rejects([&] { (void)pcc::emit_smt_proof(corrupted); }, "wrong transformation kind in PCC record");
  records.front().kind = pcc::TransformKind::BranchFolding;
  records.front().dependencies.pop_back();
  rejects([&] { (void)pcc::emit_smt_proof(corrupted); }, "wrong provenance in PCC record");
  corrupted = branch;
  auto& replay_limits = const_cast<pcc::RewriteConstraints&>(corrupted.constraints()).limits;
  replay_limits.max_query_work = 1;
  rejects([&] { (void)pcc::emit_smt_proof(corrupted); }, "exhausted replay cannot emit a partial proof");

  ccdb::Database database(ir::program(), [](const auto& snapshot) { ir::verify(snapshot); });
  const auto snapshot = ir::publish(database, database.snapshot(), conditional, "conditional fixture");
  query::Options options;
  options.conditional_constants = false;
  const auto disabled = query::optimize(snapshot, options);
  require(disabled.rewrites().empty() && pcc::emit_smt_proof(disabled).obligations.empty(),
          "disabled conditional propagation must not be resurrected during Candidate replay");
  options.conditional_constants = true;
  options.analysis_work = 1;
  const auto exhausted = query::optimize(snapshot, options);
  require(exhausted.budget_exhausted() && exhausted.rewrites().empty(),
          "SCCP must not publish a provisional constant on budget exhaustion");

  const auto malformed =
      pcc::validate_smtlib2_syntax("(set-logic QF_UF)\n(assert");
  if (pcc::satie_smtlib2_available())
    require(malformed.status == pcc::SMTSyntaxStatus::Invalid,
            "Satie must reject malformed proof text");
  else
    require(malformed.status == pcc::SMTSyntaxStatus::Unknown,
            "syntax status must be Unknown without Satie");
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("expected one PCC test suite");
    const std::string suite = argv[1];
    if (suite == "rewrite")
      rewrite_tests();
    else if (suite == "smt")
      smt_tests();
    else if (suite == "regalloc")
      regalloc_tests();
    else if (suite == "negative")
      negative_tests();
    else
      throw std::invalid_argument("unknown PCC test suite: " + suite);
    std::cout << "PCC " << suite << " tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "PCC tests: " << error.what() << '\n';
    return 1;
  }
}
