#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mahak/Json.hpp"

namespace moonstone::mahak::coordination {

// Worker roster shared through a single JSON file so every agent can address
// its comrades by name instead of by numeric protocol id.
//
//   <root>/WORKERS.json    published roster (read-only for workers)
//   <root>/ACKS.json       per-revision acknowledgements (owned by Atlas)
//   <root>/roster.lock     writer lock for publish/acknowledge
//   <root>/session.lock    session ownership lock (held by the broker)
//
// The default root is /dev/shm/foo-blotter. Every mutation is atomic: write a
// temporary file, fsync it, rename it into place, then fsync the directory.

inline constexpr const char *default_roster_root = "/dev/shm/foo-blotter";

struct RosterIdentity {
    std::string name;      // comrade name, e.g. "Atlas"
    std::string role;      // short role token, e.g. "engine", "orchestrator"
    std::uint32_t protocol_id = 0;
    bool orchestrator = false;
};

struct RosterEntry {
    std::string name;
    std::string role;
    std::uint32_t protocol_id = 0;
    std::string inbox;    // POSIX queue name owned by this worker
    bool orchestrator = false;
};

struct Roster {
    std::uint32_t revision = 1;
    std::string session;
    std::string root;
    std::string broker_ingress;             // the orchestrator's ingress queue
    std::uint64_t published_unix_ns = 0;
    std::vector<RosterEntry> entries;

    // Name or numeric protocol id. Returns nullptr when the name is unknown.
    const RosterEntry *lookup(std::string_view name) const;
    std::string display_name(std::string_view name) const; // "" when unknown
};

std::string roster_path(std::string_view root);
std::string default_root();

// Derives queue names from the session: the orchestrator uses the "broker"
// suffix, every other worker uses "w<protocol id>". Exactly one identity must
// be the orchestrator; names, roles and protocol ids must be unique.
Result<Roster> build_roster(std::string_view root, std::string_view session,
                            const std::vector<RosterIdentity> &identities);
Result<void> publish_roster(const Roster &roster);
Result<Roster> load_roster(std::string_view root);
Result<std::optional<Roster>> try_load_roster(std::string_view root);

// Resolves a comrade name (or a numeric id) to its inbox queue name.
Result<RosterEntry> resolve_worker(const Roster &roster, std::string_view name);
// Resolves a comrade name to the numeric protocol id used on the wire.
Result<std::uint32_t> resolve_protocol_id(const Roster &roster, std::string_view name);

// Acknowledges the roster revision. A stale revision (older than the currently
// published one) is rejected so a worker cannot claim a roster it never read.
Result<void> acknowledge_roster(std::string_view root, std::string_view name,
                                std::uint32_t revision);
Result<std::vector<std::string>> pending_acknowledgements(const Roster &roster);
Result<std::uint32_t> acknowledged_revision(std::string_view root);

// Session-level ownership of the roster directory, held by the orchestrator for
// the lifetime of the session. Distinct from roster.lock so publishing from the
// owning process never self-deadlocks.
class RosterLock {
  public:
    static Result<RosterLock> acquire(std::string_view root);
    ~RosterLock();
    RosterLock(RosterLock &&) noexcept;
    RosterLock &operator=(RosterLock &&) noexcept;
    RosterLock(const RosterLock &) = delete;
    RosterLock &operator=(const RosterLock &) = delete;

    const std::string &root() const noexcept;
    // Reads the owning pid recorded in session.lock (0 when absent).
    Result<long long> owner_pid() const;

  private:
    RosterLock(std::string root, int fd);
    std::string root_;
    int fd_ = -1;
};

} // namespace moonstone::mahak::coordination
