#pragma once

#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Model.hpp"

namespace moonstone::mahak {
inline namespace v1 {

struct PlanOptions {
    // Empty filters select everything. Tag filters require at least one match.
    std::set<std::string> benchmarks;
    std::set<std::string> targets;
    std::set<std::string> groups;
    std::set<std::string> tags;
    std::set<ExecutionMode> modes;
    std::vector<std::pair<std::string, std::string>> parameter_overrides;
    std::uint64_t seed = 0;
    bool include_excluded = false;
    std::string work_root;
    bool require_runnable = false;
};

// Parses a JSON manifest and performs strict schema validation. Unknown keys,
// duplicate ids, invalid stage ordering, malformed placeholders and impossible
// parameter matrices are errors, not warnings.
Result<Manifest> load_manifest(std::string_view path);
Result<Manifest> parse_manifest(std::string_view text, std::string_view source,
                                std::string base_directory);
Diagnostics validate_manifest(const Manifest &manifest);

// Expands parameters and filters into concrete cases. The result is stable for
// a fixed manifest and seed; ordering is by benchmark, target, then parameter
// lexicographic order, never by hash-map iteration order.
Result<SuitePlan> plan_manifest(const Manifest &manifest, const PlanOptions &options);

// Content digest over normalized manifest data (FNV-1a 64, hex encoded).
std::string manifest_digest(const Manifest &manifest);

// Human-readable diagnostics rendering (path:line is not available for JSON
// manifests; the JSON pointer is included in the message).
std::string format_diagnostics(const Diagnostics &diagnostics);

} // namespace v1
} // namespace moonstone::mahak
