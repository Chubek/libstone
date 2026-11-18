#include "novostone/CostCC/CostCC.hpp"
#include "novostone/PCC/SMTProof.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#if defined(NOVOSTONE_PCC_SIGNED_PACKAGES)
#include "PCCPackage.hpp"
#endif

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace novostone;
namespace {
std::string read_text(const std::string& path, std::size_t limit = 16 * 1024 * 1024) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open input: " + path);
  std::string text;
  char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    auto count = static_cast<std::size_t>(input.gcount());
    if (count > limit - text.size()) throw std::invalid_argument("input byte limit exceeded: " + path);
    text.append(buffer, count);
  }
  if (!input.eof()) throw std::runtime_error("cannot read input: " + path);
  return text;
}
ir::Function read_function(const std::string& path) { return ir::parse(read_text(path)); }
std::uint32_t integer(const std::string& text) {
  std::uint32_t value;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) throw std::invalid_argument("expected unsigned i32: " + text);
  return value;
}
std::size_t size_value(const std::string& text) {
  std::size_t value;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::invalid_argument("expected nonnegative size: " + text);
  return value;
}
void select_transforms(pcc::RewriteConstraints& constraints,
                       const std::string& text) {
  constraints.allowed.clear();
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto comma = text.find(',', begin);
    const auto end = comma == std::string::npos ? text.size() : comma;
    if (end == begin)
      throw std::invalid_argument("empty PCC transformation name");
    constraints.allow(
        std::string_view(text).substr(begin, end - begin));
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
}
void write_blob(std::ostream& out, const std::string& label,
                const std::string& text) {
  out << label << ' ' << text.size() << '\n';
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (text.empty() || text.back() != '\n') out << '\n';
}
void write_pcc_package(const pcc::Artifact& artifact,
                       const pcc::SMTProofArtifact& proof) {
  std::cout << "pcc-package " << artifact.schema_version() << '\n'
            << "frontend " << pcc::name(proof.frontend) << '\n'
            << "semantics " << artifact.semantics() << '\n';
  write_blob(std::cout, "artifact-core", artifact.canonical_core());
  write_blob(std::cout, "source-ir", ir::dump(artifact.source()));
  write_blob(std::cout, "optimized-ir", ir::dump(artifact.transformed()));

  const auto& constraints = artifact.constraints();
  std::cout << "constraints\n"
            << "allowed " << constraints.allowed.size();
  for (const auto kind : constraints.allowed)
    std::cout << ' ' << pcc::name(kind);
  std::cout << "\nlimits "
            << constraints.limits.max_arity << ' '
            << constraints.limits.max_blocks << ' '
            << constraints.limits.max_instructions << ' '
            << constraints.limits.max_references << ' '
            << constraints.limits.max_rewrites << ' '
            << constraints.limits.max_query_work << ' '
            << constraints.limits.max_query_facts << ' '
            << constraints.limits.max_analysis_work << '\n';

  const auto& analysis = artifact.analysis();
  std::cout << "analysis\n"
            << "work " << analysis.source_analysis_work << ' '
            << analysis.optimizer_query_work << ' '
            << analysis.optimizer_analysis_work << '\n'
            << "values " << analysis.values.size() << '\n';
  for (const auto& value : analysis.values)
    std::cout << value.value << ' '
              << static_cast<std::uint32_t>(value.state) << ' '
              << value.constant << '\n';
  std::cout << "blocks " << analysis.executable_blocks.size();
  for (const auto block : analysis.executable_blocks)
    std::cout << ' ' << block;
  std::cout << "\nedges " << analysis.executable_edges.size() << '\n';
  for (const auto& edge : analysis.executable_edges)
    std::cout << edge.from << ' ' << edge.to << '\n';

  std::cout << "transformations " << artifact.transformations().size()
            << '\n';
  for (const auto& transformation : artifact.transformations()) {
    std::cout << "transformation " << transformation.sequence << ' '
              << pcc::name(transformation.kind) << ' '
              << transformation.source_revision << ' '
              << transformation.instruction << ' '
              << transformation.dependencies.size() << '\n';
    write_blob(std::cout, "rule", transformation.rule);
    for (const auto& dependency : transformation.dependencies)
      write_blob(std::cout, "dependency", dependency);
  }

  std::cout << "smt-code " << proof.obligations.size() << '\n';
  for (const auto& obligation : proof.obligations) {
    std::cout << "obligation " << obligation.ordinal << ' '
              << obligation.identifier << ' '
              << pcc::name(obligation.status) << '\n';
    write_blob(std::cout, "claim", obligation.claim);
    write_blob(std::cout, "smtlib2", obligation.smtlib2);
  }
  std::cout << "end-pcc-package\n";
  std::cout.flush();
  if (!std::cout)
    throw std::runtime_error("cannot write PCC package");
}
}
int main(int argc, char** argv) {
  try {
#if defined(NOVOSTONE_PCC_SIGNED_PACKAGES)
    if (const auto status = tools::pcc_package_command(argc, argv))
      return *status;
#endif
    if ((argc == 4 && std::string(argv[1]) == "datalog") ||
        (argc == 3 && std::string(argv[1]) == "format-datalog")) {
      auto module = datalog::parse(read_text(argv[2], datalog::ParseLimits{}.bytes), argv[2]);
      if (argc == 3) { std::cout << datalog::dump(module); return 0; }
      ccdb::Database database(module.program);
      auto transaction = database.begin(); transaction.insert(module.facts);
      auto snapshot = transaction.commit("load Datalog source");
      std::cout << datalog::dump(snapshot.query(argv[3]).rows);
      return 0;
    }
    ccdb::Database db(ir::program(), [](const auto& s) { ir::verify(s); });
    if (argc >= 3 && std::string(argv[1]) == "pcc") {
      pcc::RewriteConstraints constraints;
      std::optional<pcc::SMTFrontend> frontend;
      bool require_proof = false;
      for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        const auto argument = [&](const char* name) -> std::string {
          if (++index == argc)
            throw std::invalid_argument(std::string(name) +
                                        " requires an argument");
          return argv[index];
        };
        if (option == "--frontend")
          frontend = pcc::parse_smt_frontend(argument("--frontend"));
        else if (option == "--transforms")
          select_transforms(constraints, argument("--transforms"));
        else if (option == "--max-rewrites")
          constraints.limits.max_rewrites =
              size_value(argument("--max-rewrites"));
        else if (option == "--require-proof")
          require_proof = true;
        else
          throw std::invalid_argument("unknown PCC option: " + option);
      }
      if (!frontend)
        throw std::invalid_argument(
            "PCC requires an explicit --frontend choice");

      auto artifact = pcc::build(read_function(argv[2]), constraints);
      auto proof = pcc::emit_smt_proof(artifact);
      pcc::discharge_smt(proof, *frontend);
      write_pcc_package(artifact, proof);
      if (require_proof && !proof.all_proved()) {
        std::cerr << "novostone: one or more PCC obligations are not proved\n";
        return 3;
      }
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "analyze") {
      auto function = read_function(argv[2]);
      auto source = ir::publish(db, db.snapshot(), function, "analysis input");
      std::cout << ir::liveness(source).explain() << query::propagate(function).explain();
      for (const auto& loop : ir::natural_loops(function)) {
        std::cout << "loop header " << loop.header << " blocks";
        for (auto block : loop.blocks) std::cout << ' ' << block;
        std::cout << " latches";
        for (auto latch : loop.latches) std::cout << ' ' << latch;
        std::cout << '\n';
      }
      return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "run") {
      auto function = read_function(argv[2]);
      std::vector<std::uint32_t> arguments;
      for (int i = 3; i < argc; ++i) arguments.push_back(integer(argv[i]));
      auto outcome = ir::evaluate(function, arguments);
      if (!outcome.trap.empty()) { std::cerr << "trap: " << outcome.trap << '\n'; return 1; }
      std::cout << "result " << outcome.value.value() << " steps " << outcome.state.steps << '\n';
      return 0;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "validate") {
      auto before = read_function(argv[2]), after = read_function(argv[3]);
      auto seed = argc == 5 ? integer(argv[4]) : 728u;
      auto report = query::validate(before, after, query::corpus(before.arity, seed));
      std::cout << "seed " << seed << '\n' << report.explain() << '\n';
      if (report.counterexample)
        std::cout << query::minimize(before, after, report.counterexample->input).explain() << '\n';
      return report.verdict == query::Verdict::Passed ? 0 : report.verdict == query::Verdict::Mismatch ? 1 : 2;
    }
    if (argc == 3 && std::string(argv[1]) == "optimize") {
      auto source = ir::publish(db, db.snapshot(), read_function(argv[2]), "parse input");
      auto candidate = query::optimize(source);
      std::cout << ir::dump(ir::load(candidate.result()));
      std::cerr << candidate.explain() << '\n';
      return 0;
    }
    if (argc != 1 && !(argc == 2 && std::string(argv[1]) == "demo")) {
      std::cerr << "usage: novostone [demo | analyze FILE | optimize FILE | "
                   "pcc FILE --frontend smtlib2 [--transforms LIST] "
                   "[--max-rewrites N] [--require-proof] | "
                   "run FILE [I32...] | validate BEFORE AFTER [SEED] | "
                   "datalog FILE RELATION | format-datalog FILE]\n";
#if defined(NOVOSTONE_PCC_SIGNED_PACKAGES)
      std::cerr << "       novostone pcc-pack FILE --sign-key PEM "
                   "[--transforms LIST] [--max-rewrites N]\n"
                   "       novostone pcc-verify PACKAGE --trust-key PEM\n"
                   "       novostone pcc-serve --trust-key PEM\n";
#endif
      return 2;
    }
    ir::Builder builder(db, 1);
    auto block = builder.block();
    auto x = builder.emit(block, ir::Op::Param, {}, 0);
    auto zero = builder.emit(block, ir::Op::Const, {}, 0);
    auto sum = builder.emit(block, ir::Op::Add, {x, zero});
    builder.emit(block, ir::Op::Return, {sum});
    auto source = builder.finish();
    auto optimized = query::optimize(source);
    cost::Ranker ranker;
    std::vector<query::Candidate> candidates{query::baseline(source), optimized};
    auto decision = ranker.choose(candidates, {"reference-i32", "scalar", "none", "demo", 1});
    std::cout << optimized.explain() << '\n' << decision.explain();
    auto assumptions = std::make_shared<speculative::Assumptions>();
    auto assumption = assumptions->assume("argument usually equals 42");
    auto version = speculative::Version::specialize(source, assumptions, {{x, 42, assumption}});
    for (auto value : {42u, 7u}) {
      auto execution = version.execute({value});
      std::cout << "input " << value << " result " << execution.outcome.value.value()
                << " deoptimized " << execution.failed_guard.has_value() << '\n';
    }
    assumptions->invalidate(assumption);
    std::cout << "after invalidation: " << version.execute({42}).reason << '\n';
    query::apply(db, candidates[decision.selected]);
    std::cout << db.snapshot().dump();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "novostone: " << error.what() << '\n';
    return 1;
  }
}
