#pragma once
#include "../../core/Core.hpp"
#include "../sentinel/ConcurrencyAnalysis.hpp"
#include <string>
#include <vector>

namespace moonstone::plaid::integration {
inline Diagnostics race_diagnostics(const std::vector<sentinel::Race> &races,
                                    std::string file = {}) {
    Diagnostics diagnostics;
    for (const auto &race : races) {
        Diagnostic d;
        d.id = "PLAID-RACE";
        d.message =
            "conflicting unordered accesses to location " + std::to_string(race.first.location);
        d.primary.file = file;
        d.notes.push_back("threads " + std::to_string(race.first.thread) + " and " +
                          std::to_string(race.second.thread));
        diagnostics.push_back(std::move(d));
    }
    return diagnostics;
}

struct CaptureContract {
    bool sendable = false;
    bool suspend_safe = false;
    bool thread_affine = false;
};
inline ProofResult check_migration(CaptureContract capture, bool can_migrate) {
    if (can_migrate && (!capture.sendable || capture.thread_affine))
        return {Status::Refuted, "captured values are not sendable or are thread-affine", {}};
    if (!capture.suspend_safe)
        return {Status::Refuted, "captured state is not suspend-safe", {}};
    return {Status::Proven, "contract permits suspension and requested migration", {}};
}
} // namespace moonstone::plaid::integration
