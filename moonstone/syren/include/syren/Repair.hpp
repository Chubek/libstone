#pragma once
#include "../../../core/Core.hpp"
namespace moonstone::syren {
enum class RepairStatus { Candidate, Rejected, Validated, Inconclusive };
struct RepairCandidate {
    std::string id, explanation;
    std::vector<SourceEdit> edits;
    std::int64_t cost = 1;
    RepairStatus status = RepairStatus::Candidate;
    std::string reason;
};
using Validator = std::function<ProofResult(std::string_view)>;
inline RepairCandidate verify_repair(RepairCandidate candidate, std::string_view source,
                                     const Validator &verify) {
    if (candidate.cost < 0 || !verify) {
        candidate.status = RepairStatus::Rejected;
        candidate.reason = "invalid cost or missing validator";
        return candidate;
    }
    auto changed = apply_edits(std::string(source), candidate.edits);
    if (!changed) {
        candidate.status = RepairStatus::Rejected;
        candidate.reason = "invalid or overlapping source edits";
        return candidate;
    }
    auto proof = verify(*changed);
    candidate.reason = proof.explanation;
    switch (proof.status) {
    case Status::Proven:
        candidate.status = RepairStatus::Validated;
        break;
    case Status::Refuted:
    case Status::InvalidInput:
        candidate.status = RepairStatus::Rejected;
        break;
    default:
        candidate.status = RepairStatus::Inconclusive;
        break;
    }
    return candidate;
}
inline std::vector<RepairCandidate> rank_repairs(std::vector<RepairCandidate> xs) {
    auto rank = [](RepairStatus status) {
        switch (status) {
        case RepairStatus::Validated:
            return 0;
        case RepairStatus::Candidate:
            return 1;
        case RepairStatus::Inconclusive:
            return 2;
        case RepairStatus::Rejected:
            return 3;
        }
        return 4;
    };
    std::stable_sort(xs.begin(), xs.end(), [&](const auto &a, const auto &b) {
        if (rank(a.status) != rank(b.status))
            return rank(a.status) < rank(b.status);
        if (a.cost != b.cost)
            return a.cost < b.cost;
        return a.id < b.id;
    });
    return xs;
}
} // namespace moonstone::syren
