#pragma once

#include "novostone/PCC/RegisterAllocation.hpp"
#include "novostone/QueryBasedIR/QueryBasedIR.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace novostone::pcc {

// The artifact describes Novostone's one-word IR semantics.  In particular,
// add/sub/mul are operations on bit patterns modulo 2^32, not signed C++
// arithmetic and not mathematical integers.
inline constexpr std::uint32_t artifact_schema_version = 1;
inline constexpr std::string_view i32_semantics =
  "novostone.ir.i32.modulo-2^32.v1";
inline constexpr std::string_view smtlib2_frontend = "smt-lib2";

// Numeric values are stable artifact field values.  New kinds must be appended.
enum class TransformKind : std::uint32_t {
  ConstantFolding = 1,
  LocalCSE = 2,
  AlgebraicSimplification = 3,
  CopyPropagation = 4,
  DeadCodeElimination = 5,
  BranchFolding = 6,
  PhiSimplification = 7,
  LoadForwarding = 8,
  ConditionalConstantPropagation = 9,
};

std::string_view name(TransformKind kind);
TransformKind transform_kind(std::string_view name);
// The stable kind assigned to a built-in QueryBasedIR rule name.
TransformKind rewrite_kind(std::string_view rule);
const std::set<TransformKind>& supported_transform_kinds();

struct WorkLimits {
  // Structural caps are checked before the ordinary IR verifier is invoked.
  std::size_t max_arity = 1'000'000;
  std::size_t max_blocks = 100'000;
  std::size_t max_instructions = 1'000'000;
  std::size_t max_references = 4'000'000;

  // QueryBasedIR has independent deterministic work-unit limits.  These are
  // not wall-clock deadlines.  A policy-prefix reconstruction can run the
  // optimizer a second time, so total optimizer work is at most twice these
  // per-run limits and is reported in Analysis.
  std::size_t max_rewrites = 1'000;
  std::size_t max_query_work = 10'000'000;
  std::size_t max_query_facts = 1'000'000;
  std::size_t max_analysis_work = 1'000'000;
  bool operator==(const WorkLimits&) const = default;
};

struct RewriteConstraints {
  // Conservative PCC default: only the two transformations required by the
  // core pipeline are eligible.  Call allow() for other built-in QueryBasedIR
  // transformations, or use all_query_rewrites().
  std::set<TransformKind> allowed{
    TransformKind::ConstantFolding,
    TransformKind::LocalCSE,
  };
  WorkLimits limits;

  bool allows(TransformKind kind) const;
  void allow(TransformKind kind);
  void allow(std::string_view kind);
  void disallow(TransformKind kind);
  void disallow(std::string_view kind);

  static RewriteConstraints all_query_rewrites();
  bool operator==(const RewriteConstraints&) const = default;
};

enum class AbstractState : std::uint32_t {
  Unknown = 0,
  Constant = 1,
  Overdefined = 2,
};

struct ValueFact {
  ir::Id value = 0;
  AbstractState state = AbstractState::Unknown;
  // Meaningful only when state == Constant; otherwise canonicalized to zero.
  std::uint32_t constant = 0;
  bool operator==(const ValueFact&) const = default;
};

struct EdgeFact {
  ir::Id from = 0;
  ir::Id to = 0;
  bool operator==(const EdgeFact&) const = default;
};

struct Analysis {
  // A complete source SCCP fixed point.  The pipeline throws on SCCP resource
  // exhaustion rather than returning provisional facts.
  std::vector<ValueFact> values;
  std::vector<ir::Id> executable_blocks;
  std::vector<EdgeFact> executable_edges;
  std::size_t source_analysis_work = 0;

  // Cumulative counters for optimizer discovery and (when needed) rebuilding
  // the accepted policy prefix.
  std::size_t optimizer_query_work = 0;
  std::size_t optimizer_analysis_work = 0;
  bool operator==(const Analysis&) const = default;
};

struct Transformation {
  std::size_t sequence = 0;
  TransformKind kind = TransformKind::ConstantFolding;
  ccdb::Revision source_revision = 0;
  ir::Id instruction = 0;
  std::string rule;
  std::vector<std::string> dependencies;
  bool operator==(const Transformation&) const = default;
};

enum class StopCause : std::uint32_t {
  // The configured QueryBasedIR run found no additional eligible rewrite.
  NoFurtherRewrite = 0,
  // The next deterministic rewrite was not enabled by RewriteConstraints.
  ConstraintBoundary = 1,
  // A configured QueryBasedIR work or rewrite bound was reached.
  ResourceLimit = 2,
};

struct Selection {
  StopCause stop = StopCause::NoFurtherRewrite;
  std::string detail;
  std::optional<Transformation> blocked;
  bool operator==(const Selection&) const = default;
};

// Evidence attachments are deliberately non-authoritative.  PCC bounds their
// shape and stores them with this artifact, but does not invoke a solver,
// validate a proof, validate an allocation, or expose a positive trust bit.
struct SmtProofText {
  std::string frontend{std::string(smtlib2_frontend)};
  std::string logic;
  std::string producer;
  std::string obligation;
  std::string proof;
  bool operator==(const SmtProofText&) const = default;
};

class Artifact {
public:
  std::uint32_t schema_version() const noexcept { return schema_version_; }
  std::string_view semantics() const noexcept { return semantics_; }
  const RewriteConstraints& constraints() const noexcept { return constraints_; }
  const ir::Function& source() const noexcept { return source_; }
  const Analysis& analysis() const noexcept { return analysis_; }
  const std::vector<Transformation>& transformations() const noexcept {
    return transformations_;
  }
  const Selection& selection() const noexcept { return selection_; }
  const ir::Function& transformed() const noexcept { return transformed_; }

  const std::optional<SmtProofText>& smt_proof_text() const noexcept {
    return smt_proof_text_;
  }
  const std::optional<RegisterAllocationEvidence>&
  register_allocation_evidence() const noexcept {
    return register_allocation_evidence_;
  }

  void attach_smt_proof_text(SmtProofText evidence);
  void attach_register_allocation_evidence(RegisterAllocationEvidence evidence);
  void clear_smt_proof_text() noexcept { smt_proof_text_.reset(); }
  void clear_register_allocation_evidence() noexcept {
    register_allocation_evidence_.reset();
  }

  // Canonical, deterministic core representation used as the subject of
  // downstream obligations.  Attachments are excluded so adding solver or
  // register-allocation output cannot change the obligation's subject.
  [[nodiscard]] std::string canonical_core() const;
  [[nodiscard]] std::string explain() const;

private:
  Artifact(RewriteConstraints constraints, ir::Function source,
           Analysis analysis, std::vector<Transformation> transformations,
           Selection selection, ir::Function transformed);

  std::uint32_t schema_version_ = artifact_schema_version;
  std::string semantics_{i32_semantics};
  RewriteConstraints constraints_;
  ir::Function source_;
  Analysis analysis_;
  std::vector<Transformation> transformations_;
  Selection selection_;
  ir::Function transformed_;
  std::optional<SmtProofText> smt_proof_text_;
  std::optional<RegisterAllocationEvidence> register_allocation_evidence_;

  friend Artifact build(const ir::Function&, const RewriteConstraints&);
};

// Constructs a deterministic candidate artifact.  This function establishes
// structural validity and uses only QueryBasedIR-authorized rewrites.  It does
// not claim semantic proof: downstream integration may attach independently
// produced SMT-LIB2 and register-allocation evidence.
[[nodiscard]] Artifact build(const ir::Function& source,
                             const RewriteConstraints& constraints);

} // namespace novostone::pcc
