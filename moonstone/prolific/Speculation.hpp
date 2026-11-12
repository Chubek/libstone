#pragma once
#include "Prolific.hpp"
#include <optional>

namespace moonstone::prolific {

enum class ProfileKind : std::uint8_t {
    Type,
    Branch,
    Value,
    Call,
    Interoperation,
    System,
    Cost,
    Lowering,
    Allocation,
    Exception,
    Custom
};
const char *name(ProfileKind kind);
bool numeric(ProfileKind kind);

struct Observation {
    ProfileKind kind = ProfileKind::Type;
    std::string site, context, dimension;
    std::string outcome;    // categorical UTF-8 identity; branch: "true" or "false"
    std::string unit;       // numeric only, e.g. "i64", "ns", "bytes"
    std::int64_t value = 0; // numeric only; cost/allocation must be nonnegative
};

// Same blocking/thread/lifetime contract as Recorder::emit. Invalid observations
// count as recording loss. Payloads use existing cacheable v1 Instant events.
Code observe(Recorder &recorder, Domain domain, const Observation &observation,
             Location location = {});
// nullopt for ordinary events; throws for malformed/unsupported reserved events.
std::optional<Observation> profile_observation(const Event &event);

struct SpeculationPolicy {
    std::uint64_t minimum_samples = 32;
    unsigned dominance_permille = 950; // 501..1000; empirical frequency, not a proof
    std::size_t profiles = 1024, buckets_per_profile = 64, instructions = 8192;
    std::size_t output_bytes = 16 * 1024 * 1024;
};

// Returns a complete Lua SpecIR module. Uses only supplied observations;
// never authorizes a transformation. Throws on invalid input/global exhaustion.
// Incomplete/truncated/insufficient evidence emits warnings, not candidates.
// Thread-safe for immutable snapshots; synchronous and potentially allocating.
std::string speculate(const Snapshot &snapshot, SpeculationPolicy policy = {});

} // namespace moonstone::prolific
