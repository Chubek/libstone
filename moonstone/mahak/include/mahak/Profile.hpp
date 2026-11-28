#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"
#include "Report.hpp"
#include "Storage.hpp"

// Durable benchmark profiles: a named history of sessions that grows over time
// and is the input to trend reporting and to recommendations.
//
// Canonical session documents stay in HistoryStore JSON. A profile is an index
// over those documents plus a cached execution fingerprint, so a profile can be
// rebuilt from canonical records and a secondary store (LMDB) is never the only
// copy of a result.

namespace moonstone::mahak {
inline namespace v1 {

inline constexpr std::uint32_t mahak_profile_schema_version = 1;

// Stable identity is separate from the display name: renaming a profile must
// not orphan its history, and two profiles may share a display name.
struct ProfileId {
    std::string stable;  // [a-z0-9][a-z0-9._-]*, used as a directory name
    std::string display;
};

bool valid_profile_id(std::string_view stable) noexcept;
Result<ProfileId> make_profile_id(std::string_view display);

// Everything that must match before two sessions may be compared or trended.
// A replay of a cached execution must match this fingerprint; a genuine second
// execution is a new session even when the fingerprint matches.
struct ExecutionFingerprint {
    bool valid = false;
    std::string digest;  // FNV-1a 64 hex over the normalized components
    Json components = Json::object();
};

struct FingerprintInput {
    std::string executable_digest;
    std::vector<std::string> argv;
    std::string fixture_digest;
    std::string runner_digest;
    std::vector<std::string> plugin_ids;
    std::uint64_t seed = 0;
    std::string oracle;
    std::string measurement_policy;
    std::string compiler_configuration;
    std::string target_abi;
    std::string environment_digest;
};

Result<ExecutionFingerprint> fingerprint_execution(const FingerprintInput &input);

// Replay reuses a stored session's samples and adds none; fresh always runs.
enum class ReplayPolicy { Fresh, Replay };

struct ReplayDecision {
    ReplayPolicy policy = ReplayPolicy::Fresh;
    std::string source_session;  // empty when policy is Fresh
    std::string reason;          // why the cache did or did not apply
};

struct ProfileOptions {
    std::string root;  // directory holding profile.json and the session store
    std::string name;  // display name; also the source of a new stable id
};

struct ProfileSession {
    std::string session_id;
    std::uint64_t created_unix_ns = 0;
    std::string status;
    std::size_t runs = 0;
    ExecutionFingerprint fingerprint;
};

struct DotOptions {
    bool chronological = true;
    bool label_uncertainty = true;
    bool distinguish_verdicts = true;
    std::size_t max_sessions = 200;
    std::vector<std::string> metrics;  // empty means every metric present
};

// Deterministic Graphviz output for one profile. Stable session ids, escaped
// labels, units, and visibly distinct regression/improvement/inconclusive
// edges. The same input always produces byte-identical output.
std::string render_dot(const std::vector<SessionRecord> &sessions, const DotOptions &options = {});

class Profile {
  public:
    static Result<Profile> open(const ProfileOptions &options);
    ~Profile();
    Profile(Profile &&) noexcept;
    Profile &operator=(Profile &&) noexcept;
    Profile(const Profile &) = delete;
    Profile &operator=(const Profile &) = delete;

    const ProfileId &id() const noexcept;
    const std::string &root() const noexcept;

    // Commits a finished session and its fingerprint to the canonical store.
    // Idempotent for an identical document; a conflicting document is rejected.
    Result<void> commit(const SessionRecord &session, const ExecutionFingerprint &fingerprint);

    Result<std::vector<ProfileSession>> sessions() const;
    Result<SessionRecord> load(std::string_view session_id) const;

    // Returns a ReplayDecision for a fresh run request. `policy` decides whether
    // a fingerprint match is even considered.
    Result<ReplayDecision> decide_replay(const ExecutionFingerprint &fingerprint,
                                         ReplayPolicy policy) const;

    // Loads committed sessions in creation order for trend rendering. Sessions
    // with an incompatible fingerprint are included and labelled, never dropped.
    Result<std::vector<SessionRecord>> history(std::size_t max_sessions = 0) const;

    Result<std::string> render_dot(const DotOptions &options = {}) const;
    Result<std::string> render_history(std::string_view metric,
                                       const ReportOptions &options = {}) const;

    // Rebuilds the derived index from canonical session documents.
    Result<void> reindex();

  private:
    Profile();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace moonstone::mahak
