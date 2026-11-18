#pragma once

#include "novostone/PCC/PCC.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace novostone::pcc {

// The PCC command-line/configuration spelling is deliberately narrower than
// the set of solver APIs that may happen to be present in the source tree.
enum class SMTFrontend {
  SMTLIB2
};

SMTFrontend parse_smt_frontend(std::string_view name);
std::string_view name(SMTFrontend frontend);

// Each obligation contains the negation of its correctness claim.  Therefore
// UNSAT proves the encoded part of the rewrite, while SAT refutes that local
// claim. Trust also requires this emitter's exact edit/structural checks and
// the IR verifier. An arbitrary SMT string or mutable status is not a proof
// of a specific IR pair; regenerate and discharge the matching obligations.
enum class ObligationStatus {
  Unchecked,
  UnsatProved,
  SatCounterexample,
  Unknown
};

std::string_view name(ObligationStatus status);

enum class SMTSyntaxStatus {
  Valid,
  Invalid,
  Unknown
};

std::string_view name(SMTSyntaxStatus status);

// Hex-encodes every input byte into a simple SMT-LIB symbol.  This is
// injective and does not rely on quoted-symbol escaping rules.
std::string smt_identifier(std::string_view text);

struct SMTProofObligation {
  std::size_t ordinal = 0;
  ccdb::Revision revision = 0;
  ir::Id instruction = 0;
  std::string rule;
  std::string identifier;
  std::string claim;
  std::string logic;
  std::string encoding;
  std::string smtlib2;
  ObligationStatus status = ObligationStatus::Unchecked;
  std::string diagnostic;
  std::map<std::string, bool> counterexample;
};

struct SMTProofArtifact {
  SMTFrontend frontend = SMTFrontend::SMTLIB2;
  std::vector<SMTProofObligation> obligations;

  bool all_proved() const;
};

struct SMTSyntaxResult {
  SMTSyntaxStatus status = SMTSyntaxStatus::Unknown;
  std::string diagnostic;
};

struct SMTConduitResult {
  SMTSyntaxStatus syntax = SMTSyntaxStatus::Unknown;
  ObligationStatus status = ObligationStatus::Unknown;
  std::string diagnostic;
  std::map<std::string, bool> counterexample;
};

// Emits one local proof obligation.  `before` and `after` must be the exact
// adjacent verified IR states for `rewrite`; mismatched, unknown, and
// unsupported edits are rejected. This low-level API checks the semantic
// edit, not its optimizer provenance (revision and dependencies); use the
// replay APIs to validate those fields. Memory is fixed-size, sequential and
// non-volatile, and loads only forward from earlier same-block accesses.
// Execution hooks, bounded interpreter step counts, guarded/deoptimizing IR
// and native lowering/ABI semantics are outside these equivalence claims.
SMTProofObligation emit_smt_obligation(const ir::Function& before,
                                       const ir::Function& after,
                                       const query::Rewrite& rewrite,
                                       std::size_t ordinal = 0);

// Replays the committed Candidate prefix under its recorded options, checks
// each record and both endpoints, and emits an obligation for each rewrite.
// SCCP facts are checked as an inductive invariant, not assumed on the
// optimizer's authority. No partial proof artifact is returned on replay or
// proof-generation exhaustion. A complete bounded optimization prefix can
// still be proved; this does not prove optimizer completion.
SMTProofArtifact emit_smt_proof(const query::Candidate& candidate);

// The endpoint-taking overload additionally verifies that the supplied
// functions are exactly the Candidate's base and result.
SMTProofArtifact emit_smt_proof(const ir::Function& before,
                                 const ir::Function& after,
                                 const query::Candidate& candidate);

// Reconstructs the exact accepted prefix represented by a PCC core artifact,
// verifies every recorded transformation and both IR endpoints, then emits
// the corresponding proof obligations.  An artifact containing a rewrite
// without a sound supported obligation is rejected as a whole.
SMTProofArtifact emit_smt_proof(const Artifact& artifact);

// These hooks use SatieFrontendSMTLIB2 only when the translation unit is
// compiled with NOVOSTONE_PCC_USE_SATIE and Satie is linked by the caller.
// Merely having vendored headers does not silently enable solver authority.
bool satie_smtlib2_available() noexcept;
SMTSyntaxResult validate_smtlib2_syntax(std::string_view script);
SMTConduitResult run_smtlib2(std::string_view script);
SMTConduitResult run_smtlib2(const SMTProofObligation& obligation);

// Updates status/diagnostic/model in place.  Unsupported frontends are
// rejected; absent Satie or an unsupported Satie theory produces Unknown.
SMTConduitResult discharge_smt(SMTProofObligation& obligation,
                               SMTFrontend frontend = SMTFrontend::SMTLIB2);
void discharge_smt(SMTProofArtifact& artifact,
                   SMTFrontend frontend = SMTFrontend::SMTLIB2);

} // namespace novostone::pcc
