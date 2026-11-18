#include "novostone/PCC/PCC.hpp"

#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace novostone::pcc {
namespace {

constexpr std::size_t hard_max_arity = 4'000'000;
constexpr std::size_t hard_max_blocks = 1'000'000;
constexpr std::size_t hard_max_instructions = 8'000'000;
constexpr std::size_t hard_max_references = 32'000'000;
constexpr std::size_t hard_max_rewrites = 1'000'000;
constexpr std::size_t hard_max_query_work = 1'000'000'000;
constexpr std::size_t hard_max_query_facts = 100'000'000;
constexpr std::size_t hard_max_analysis_work = 1'000'000'000;
constexpr std::size_t max_attachment_field_bytes = 4'096;
constexpr std::size_t max_attachment_text_bytes = 64 * 1024 * 1024;

bool valid_kind(TransformKind kind) {
  switch (kind) {
    case TransformKind::ConstantFolding:
    case TransformKind::LocalCSE:
    case TransformKind::AlgebraicSimplification:
    case TransformKind::CopyPropagation:
    case TransformKind::DeadCodeElimination:
    case TransformKind::BranchFolding:
    case TransformKind::PhiSimplification:
    case TransformKind::LoadForwarding:
    case TransformKind::ConditionalConstantPropagation:
      return true;
  }
  return false;
}

void require_kind(TransformKind kind) {
  if (!valid_kind(kind))
    throw std::invalid_argument("unsupported PCC transformation kind");
}

void validate_limits(const WorkLimits& limits) {
  auto positive = [](std::size_t value, const char* message) {
    if (value == 0) throw std::invalid_argument(message);
  };
  positive(limits.max_arity, "PCC arity limit must be positive");
  positive(limits.max_blocks, "PCC block limit must be positive");
  positive(limits.max_instructions,
           "PCC instruction limit must be positive");
  positive(limits.max_references, "PCC reference limit must be positive");
  positive(limits.max_query_facts,
           "PCC query fact limit must be positive");
  positive(limits.max_analysis_work,
           "PCC analysis work limit must be positive");

  if (limits.max_arity > hard_max_arity ||
      limits.max_blocks > hard_max_blocks ||
      limits.max_instructions > hard_max_instructions ||
      limits.max_references > hard_max_references ||
      limits.max_rewrites > hard_max_rewrites ||
      limits.max_query_work > hard_max_query_work ||
      limits.max_query_facts > hard_max_query_facts ||
      limits.max_analysis_work > hard_max_analysis_work)
    throw std::invalid_argument("PCC work limit exceeds the implementation cap");
}

void validate_constraints(const RewriteConstraints& constraints) {
  validate_limits(constraints.limits);
  for (auto kind : constraints.allowed) require_kind(kind);
}

void add_bounded(std::size_t& total, std::size_t amount,
                 std::size_t limit, const char* message) {
  if (amount > limit - total) throw std::invalid_argument(message);
  total += amount;
}

void validate_source_size(const ir::Function& source,
                          const WorkLimits& limits) {
  if (source.arity > limits.max_arity)
    throw std::invalid_argument("PCC source arity limit exceeded");
  if (source.blocks.size() > limits.max_blocks)
    throw std::invalid_argument("PCC source block limit exceeded");

  std::size_t instructions = 0;
  std::size_t references = 0;
  for (const auto& [id, block] : source.blocks) {
    (void)id;
    add_bounded(instructions, block.instructions.size(),
                limits.max_instructions,
                "PCC source instruction limit exceeded");
    for (const auto& instruction : block.instructions) {
      add_bounded(references, instruction.operands.size(),
                  limits.max_references,
                  "PCC source reference limit exceeded");
      add_bounded(references, instruction.targets.size(),
                  limits.max_references,
                  "PCC source reference limit exceeded");
    }
  }
}

} // namespace

TransformKind rewrite_kind(std::string_view rule) {
  if (rule == "fold_add" || rule == "fold_sub" ||
      rule == "fold_mul" || rule == "fold_eq" ||
      rule == "fold_ult")
    return TransformKind::ConstantFolding;
  if (rule == "local_cse") return TransformKind::LocalCSE;
  if (rule == "add_zero" || rule == "sub_zero" ||
      rule == "mul_one" || rule == "sub_self" ||
      rule == "eq_self" || rule == "ult_self" ||
      rule == "mul_zero" || rule == "select_value")
    return TransformKind::AlgebraicSimplification;
  if (rule == "copy_propagation")
    return TransformKind::CopyPropagation;
  if (rule == "dead_pure" || rule == "dead_pure_cycle")
    return TransformKind::DeadCodeElimination;
  if (rule == "fold_branch") return TransformKind::BranchFolding;
  if (rule == "phi_uniform") return TransformKind::PhiSimplification;
  if (rule == "forward_load") return TransformKind::LoadForwarding;
  if (rule == "conditional_constant")
    return TransformKind::ConditionalConstantPropagation;
  throw std::logic_error("QueryBasedIR emitted an unsupported rewrite rule: " +
                         std::string(rule));
}

namespace {

Transformation convert(const query::Rewrite& rewrite, std::size_t sequence) {
  return {
    sequence,
    rewrite_kind(rewrite.rule),
    rewrite.revision,
    rewrite.instruction,
    rewrite.rule,
    {rewrite.dependencies.begin(), rewrite.dependencies.end()},
  };
}

bool same_rewrite(const query::Rewrite& left,
                  const query::Rewrite& right) {
  return left.revision == right.revision &&
         left.instruction == right.instruction &&
         left.rule == right.rule &&
         left.dependencies == right.dependencies;
}

std::size_t checked_sum(std::size_t left, std::size_t right) {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    throw std::overflow_error("PCC work accounting overflow");
  return left + right;
}

AbstractState abstract_state(query::ValueKind kind) {
  switch (kind) {
    case query::ValueKind::Unknown: return AbstractState::Unknown;
    case query::ValueKind::Constant: return AbstractState::Constant;
    case query::ValueKind::Overdefined: return AbstractState::Overdefined;
  }
  throw std::logic_error("QueryBasedIR returned an invalid abstract value");
}

Analysis source_analysis(const ir::Function& source,
                         std::size_t work_budget) {
  const auto propagation = query::propagate(source, work_budget);
  Analysis result;
  result.source_analysis_work = propagation.work;
  for (const auto& [id, value] : propagation.values) {
    const auto state = abstract_state(value.kind);
    result.values.push_back(
      {id, state, state == AbstractState::Constant ? value.value : 0});
  }
  result.executable_blocks.assign(propagation.blocks.begin(),
                                  propagation.blocks.end());
  for (const auto& [from, to] : propagation.edges)
    result.executable_edges.push_back({from, to});
  return result;
}

query::Options query_options(const RewriteConstraints& constraints) {
  query::Options options;
  options.rewrites = constraints.limits.max_rewrites;
  options.query_work = constraints.limits.max_query_work;
  options.query_facts = constraints.limits.max_query_facts;
  options.analysis_work = constraints.limits.max_analysis_work;
  options.conditional_constants =
    constraints.allows(TransformKind::ConditionalConstantPropagation);
  return options;
}

void validate_text_field(std::string_view value, std::size_t limit,
                         const char* empty_message,
                         const char* size_message) {
  if (value.empty()) throw std::invalid_argument(empty_message);
  if (value.size() > limit) throw std::invalid_argument(size_message);
  if (value.find('\0') != std::string_view::npos)
    throw std::invalid_argument("PCC evidence text contains NUL");
}

std::string_view stop_name(StopCause cause) {
  switch (cause) {
    case StopCause::NoFurtherRewrite: return "no_further_rewrite";
    case StopCause::ConstraintBoundary: return "constraint_boundary";
    case StopCause::ResourceLimit: return "resource_limit";
  }
  throw std::logic_error("invalid PCC stop cause");
}

void append_blob(std::ostringstream& out, std::string_view label,
                 const std::string& value) {
  out << label << ' ' << value.size() << '\n';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  out << '\n';
}

} // namespace

std::string_view name(TransformKind kind) {
  switch (kind) {
    case TransformKind::ConstantFolding: return "constant_folding";
    case TransformKind::LocalCSE: return "local_cse";
    case TransformKind::AlgebraicSimplification:
      return "algebraic_simplification";
    case TransformKind::CopyPropagation: return "copy_propagation";
    case TransformKind::DeadCodeElimination:
      return "dead_code_elimination";
    case TransformKind::BranchFolding: return "branch_folding";
    case TransformKind::PhiSimplification: return "phi_simplification";
    case TransformKind::LoadForwarding: return "load_forwarding";
    case TransformKind::ConditionalConstantPropagation:
      return "conditional_constant_propagation";
  }
  throw std::invalid_argument("unsupported PCC transformation kind");
}

TransformKind transform_kind(std::string_view text) {
  for (auto kind : supported_transform_kinds())
    if (name(kind) == text) return kind;
  throw std::invalid_argument("unsupported PCC transformation kind: " +
                              std::string(text));
}

const std::set<TransformKind>& supported_transform_kinds() {
  static const std::set<TransformKind> kinds{
    TransformKind::ConstantFolding,
    TransformKind::LocalCSE,
    TransformKind::AlgebraicSimplification,
    TransformKind::CopyPropagation,
    TransformKind::DeadCodeElimination,
    TransformKind::BranchFolding,
    TransformKind::PhiSimplification,
    TransformKind::LoadForwarding,
    TransformKind::ConditionalConstantPropagation,
  };
  return kinds;
}

bool RewriteConstraints::allows(TransformKind kind) const {
  require_kind(kind);
  return allowed.contains(kind);
}

void RewriteConstraints::allow(TransformKind kind) {
  require_kind(kind);
  allowed.insert(kind);
}

void RewriteConstraints::allow(std::string_view kind) {
  allow(transform_kind(kind));
}

void RewriteConstraints::disallow(TransformKind kind) {
  require_kind(kind);
  allowed.erase(kind);
}

void RewriteConstraints::disallow(std::string_view kind) {
  disallow(transform_kind(kind));
}

RewriteConstraints RewriteConstraints::all_query_rewrites() {
  RewriteConstraints constraints;
  constraints.allowed = supported_transform_kinds();
  return constraints;
}

Artifact::Artifact(RewriteConstraints constraints, ir::Function source,
                   Analysis analysis,
                   std::vector<Transformation> transformations,
                   Selection selection, ir::Function transformed)
  : constraints_(std::move(constraints)),
    source_(std::move(source)),
    analysis_(std::move(analysis)),
    transformations_(std::move(transformations)),
    selection_(std::move(selection)),
    transformed_(std::move(transformed)) {}

void Artifact::attach_smt_proof_text(SmtProofText evidence) {
  if (evidence.frontend != smtlib2_frontend)
    throw std::invalid_argument(
      "PCC SMT evidence must use the selected smt-lib2 frontend");
  validate_text_field(evidence.producer, max_attachment_field_bytes,
                      "PCC SMT evidence needs a producer",
                      "PCC SMT producer field is too large");
  if (!evidence.logic.empty())
    validate_text_field(evidence.logic, max_attachment_field_bytes,
                        "PCC SMT logic cannot be empty",
                        "PCC SMT logic field is too large");
  validate_text_field(evidence.obligation, max_attachment_text_bytes,
                      "PCC SMT evidence needs an obligation",
                      "PCC SMT obligation is too large");
  validate_text_field(evidence.proof, max_attachment_text_bytes,
                      "PCC SMT evidence needs proof text",
                      "PCC SMT proof text is too large");
  smt_proof_text_ = std::move(evidence);
}

void Artifact::attach_register_allocation_evidence(
  RegisterAllocationEvidence evidence) {
  // Allocation evidence has its own typed validation API.  This attachment
  // boundary only enforces storage limits; it deliberately does not reinterpret
  // EvidenceStatus or turn it into a core-artifact trust decision.
  if (evidence.native_calling_convention_proven)
    throw std::invalid_argument(
      "PCC cannot attach a native calling-convention proof claim");
  if (evidence.assignment.size() > hard_max_instructions ||
      evidence.interference.values.size() > hard_max_instructions ||
      evidence.interference.edges.size() > hard_max_references ||
      evidence.interference.neighbors.size() > hard_max_instructions ||
      evidence.preservation_obligations.size() > hard_max_references ||
      evidence.diagnostics.size() > hard_max_references ||
      evidence.limitations.size() > hard_max_references)
    throw std::invalid_argument(
      "PCC register-allocation evidence size limit exceeded");
  std::size_t graph_references = 0;
  for (const auto& [value, neighbors] : evidence.interference.neighbors) {
    (void)value;
    add_bounded(graph_references, neighbors.size(), hard_max_references,
                "PCC register-allocation evidence size limit exceeded");
  }
  for (const auto& preservation : evidence.preservation_obligations)
    add_bounded(graph_references, preservation.values.size(),
                hard_max_references,
                "PCC register-allocation evidence size limit exceeded");

  if (evidence.smtlib2.size() > max_attachment_text_bytes)
    throw std::invalid_argument(
      "PCC register-allocation evidence text limit exceeded");
  std::size_t text_bytes = evidence.smtlib2.size();
  auto account = [&](std::string_view text) {
    if (text.size() > max_attachment_text_bytes - text_bytes)
      throw std::invalid_argument(
        "PCC register-allocation evidence text limit exceeded");
    text_bytes += text.size();
  };
  for (const auto& diagnostic : evidence.diagnostics) account(diagnostic);
  for (const auto& limitation : evidence.limitations) account(limitation);
  for (const auto& preservation : evidence.preservation_obligations)
    account(preservation.predicate);
  if (text_bytes > max_attachment_text_bytes)
    throw std::invalid_argument(
      "PCC register-allocation evidence text limit exceeded");
  register_allocation_evidence_ = std::move(evidence);
}

std::string Artifact::canonical_core() const {
  // ir::dump also rechecks both functions, preventing canonicalization of a
  // caller-corrupted object.
  const auto source_text = ir::dump(source_);
  const auto transformed_text = ir::dump(transformed_);

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "novostone-pcc-core " << schema_version_ << '\n';
  append_blob(out, "semantics", semantics_);

  out << "allowed " << constraints_.allowed.size();
  for (auto kind : constraints_.allowed)
    out << ' ' << static_cast<std::uint32_t>(kind);
  out << "\nlimits " << constraints_.limits.max_arity << ' '
      << constraints_.limits.max_blocks << ' '
      << constraints_.limits.max_instructions << ' '
      << constraints_.limits.max_references << ' '
      << constraints_.limits.max_rewrites << ' '
      << constraints_.limits.max_query_work << ' '
      << constraints_.limits.max_query_facts << ' '
      << constraints_.limits.max_analysis_work << '\n';

  out << "analysis-work " << analysis_.source_analysis_work << ' '
      << analysis_.optimizer_query_work << ' '
      << analysis_.optimizer_analysis_work << '\n';
  out << "values " << analysis_.values.size() << '\n';
  for (const auto& fact : analysis_.values)
    out << fact.value << ' ' << static_cast<std::uint32_t>(fact.state)
        << ' ' << fact.constant << '\n';
  out << "blocks " << analysis_.executable_blocks.size();
  for (auto block : analysis_.executable_blocks) out << ' ' << block;
  out << "\nedges " << analysis_.executable_edges.size() << '\n';
  for (const auto& edge : analysis_.executable_edges)
    out << edge.from << ' ' << edge.to << '\n';

  out << "transformations " << transformations_.size() << '\n';
  for (const auto& transformation : transformations_) {
    out << transformation.sequence << ' '
        << static_cast<std::uint32_t>(transformation.kind) << ' '
        << transformation.source_revision << ' '
        << transformation.instruction << ' '
        << transformation.dependencies.size() << '\n';
    append_blob(out, "rule", transformation.rule);
    for (const auto& dependency : transformation.dependencies)
      append_blob(out, "dependency", dependency);
  }

  out << "selection " << static_cast<std::uint32_t>(selection_.stop)
      << '\n';
  append_blob(out, "selection-detail", selection_.detail);
  out << "blocked " << (selection_.blocked ? 1 : 0) << '\n';
  if (selection_.blocked) {
    const auto& blocked = *selection_.blocked;
    out << blocked.sequence << ' '
        << static_cast<std::uint32_t>(blocked.kind) << ' '
        << blocked.source_revision << ' ' << blocked.instruction << ' '
        << blocked.dependencies.size() << '\n';
    append_blob(out, "blocked-rule", blocked.rule);
    for (const auto& dependency : blocked.dependencies)
      append_blob(out, "blocked-dependency", dependency);
  }

  append_blob(out, "source-ir", source_text);
  append_blob(out, "transformed-ir", transformed_text);
  out << "end\n";
  return out.str();
}

std::string Artifact::explain() const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "Novostone PCC candidate; semantics " << semantics_
      << "; " << transformations_.size() << " accepted transformations"
      << "; stop " << stop_name(selection_.stop);
  if (!selection_.detail.empty()) out << ": " << selection_.detail;
  out << "\nsource SCCP work " << analysis_.source_analysis_work
      << "; optimizer query work " << analysis_.optimizer_query_work
      << "; optimizer analysis work " << analysis_.optimizer_analysis_work;
  out << "\nSMT and register-allocation attachments are non-authoritative; "
         "this artifact asserts no proof status.";
  return out.str();
}

Artifact build(const ir::Function& source,
               const RewriteConstraints& constraints) {
  validate_constraints(constraints);
  validate_source_size(source, constraints.limits);
  ir::verify(source);

  auto analysis =
    source_analysis(source, constraints.limits.max_analysis_work);

  // Import into a private database so caller state and revisions cannot affect
  // deterministic rewrite choice.
  ccdb::Database database(ir::program(),
                          [](const auto& snapshot) { ir::verify(snapshot); });
  const auto imported =
    ir::publish(database, database.snapshot(), source, "PCC source");

  auto options = query_options(constraints);
  const auto discovered = query::optimize(imported, options);

  std::size_t accepted_count = discovered.rewrites().size();
  std::optional<Transformation> blocked;
  for (std::size_t index = 0; index < discovered.rewrites().size(); ++index) {
    const auto transformation = convert(discovered.rewrites()[index], index);
    if (!constraints.allows(transformation.kind)) {
      accepted_count = index;
      blocked = transformation;
      break;
    }
  }

  std::vector<query::Rewrite> accepted;
  ir::Function transformed;
  std::size_t query_work = discovered.query_work();
  std::size_t optimizer_analysis_work = discovered.analysis_work();

  if (!blocked) {
    accepted = discovered.rewrites();
    transformed = ir::load(discovered.result());
  } else if (accepted_count == 0) {
    transformed = source;
  } else {
    // Candidate does not expose intermediate snapshots.  Re-running with the
    // exact accepted length reconstructs the deterministic accepted prefix.
    // Any mismatch is a fail-closed indication that QueryBasedIR ceased to be
    // deterministic under identical source and work limits.
    options.rewrites = accepted_count;
    const auto prefix = query::optimize(imported, options);
    if (prefix.rewrites().size() != accepted_count)
      throw std::logic_error(
        "QueryBasedIR could not reconstruct the PCC rewrite prefix");
    for (std::size_t index = 0; index < accepted_count; ++index)
      if (!same_rewrite(prefix.rewrites()[index],
                        discovered.rewrites()[index]))
        throw std::logic_error(
          "QueryBasedIR produced a non-deterministic PCC rewrite prefix");
    accepted = prefix.rewrites();
    transformed = ir::load(prefix.result());
    query_work = checked_sum(query_work, prefix.query_work());
    optimizer_analysis_work =
      checked_sum(optimizer_analysis_work, prefix.analysis_work());
  }

  std::vector<Transformation> transformations;
  transformations.reserve(accepted.size());
  for (std::size_t index = 0; index < accepted.size(); ++index) {
    auto transformation = convert(accepted[index], index);
    if (!constraints.allows(transformation.kind))
      throw std::logic_error(
        "PCC accepted a rewrite forbidden by its constraints");
    transformations.push_back(std::move(transformation));
  }

  ir::verify(transformed);
  if (transformed.arity != source.arity)
    throw std::logic_error("QueryBasedIR changed the PCC function signature");

  analysis.optimizer_query_work = query_work;
  analysis.optimizer_analysis_work = optimizer_analysis_work;

  Selection selection;
  if (blocked) {
    selection.stop = StopCause::ConstraintBoundary;
    selection.detail =
      "next QueryBasedIR rewrite is disabled: " + blocked->rule;
    selection.blocked = std::move(blocked);
  } else if (discovered.budget_exhausted()) {
    selection.stop = StopCause::ResourceLimit;
    selection.detail = discovered.stop_reason();
  } else {
    selection.stop = StopCause::NoFurtherRewrite;
    selection.detail =
      "configured QueryBasedIR run found no additional eligible rewrite";
  }

  return Artifact(constraints, source, std::move(analysis),
                  std::move(transformations), std::move(selection),
                  std::move(transformed));
}

} // namespace novostone::pcc
