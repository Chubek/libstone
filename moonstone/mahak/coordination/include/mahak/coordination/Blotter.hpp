#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mahak/Json.hpp"

namespace moonstone::mahak::coordination {

// Shared institutional memory for one development session:
//
//   /dev/shm/mahak-<session>/manifest.json
//   /dev/shm/mahak-<session>/journal.ndjson
//   /dev/shm/mahak-<session>/workers/<worker>.json
//   /dev/shm/mahak-<session>/contracts/<name>.json
//   /dev/shm/mahak-<session>/contracts/<name>.r<N>.json
//   /dev/shm/mahak-<session>/decisions.ndjson
//   /dev/shm/mahak-<session>/verdicts/<id>.json
//   /dev/shm/mahak-<session>/session.lock
//
// All mutations hold an exclusive flock on session.lock and use temp-file +
// fsync + rename. The journal is append-only and is the revision history.
class Blotter {
  public:
    static Result<Blotter> create(std::string session, std::vector<std::string> workers,
                                  std::string parent = "/dev/shm");
    static Result<Blotter> attach(std::string root);
    ~Blotter();
    Blotter(Blotter &&) noexcept;
    Blotter &operator=(Blotter &&) noexcept;
    Blotter(const Blotter &) = delete;
    Blotter &operator=(const Blotter &) = delete;

    const std::string &root() const noexcept;
    const std::string &session() const noexcept;

    Result<Json> manifest() const;
    Result<std::uint64_t> revision() const;

    // Append a journal event. `kind` is a short stable token (status, contract,
    // decision, verdict, error, worker-start, worker-stop, ...).
    Result<std::uint64_t> record_event(std::string_view actor, std::string_view kind,
                                       const Json &data);

    // Per-worker status records are atomically replaced; the journal retains
    // every previous value, so history is never silently lost.
    Result<void> update_worker(std::string_view worker, const Json &status);
    Result<Json> worker(std::string_view worker) const;
    Result<std::vector<Json>> workers() const;

    // Contracts are create-only at revision 1. A changed contract creates a new
    // revision file; it never overwrites history. The latest content is also
    // available at contracts/<name>.json for simple readers.
    Result<std::uint32_t> publish_contract(std::string_view name, const Json &contract);
    Result<Json> contract(std::string_view name) const;

    Result<void> record_decision(const Json &decision);
    Result<void> record_verdict(std::string_view id, const Json &verdict);
    Result<Json> verdict(std::string_view id) const;

    Result<std::vector<Json>> read_journal() const;
    Result<std::vector<Json>> read_decisions() const;

    // Large payloads are written here and referenced by a small queue message.
    // This is required by hosts whose total mqueue memory budget is small.
    Result<std::string> spool_payload(const Json &payload);
    Result<Json> resolve_spool(std::string_view reference) const;

    // Copies the complete blotter to a destination outside /dev/shm and writes
    // SNAPSHOT.json with session metadata and revision. Existing destination
    // content is rejected.
    Result<void> snapshot(std::string_view destination) const;

    // Removes this session directory. Refuses if the manifest session does not
    // match, if the path is not a /dev/shm/mahak-* directory, or if the path
    // contains a symlink component. Intended for orderly session cleanup.
    Result<void> cleanup();

    static Result<Json> inspect(std::string_view root);

  private:
    Blotter(std::string root, std::string session);
    Result<void> initialize();
    Result<void> write_atomic(std::string_view relative, std::string_view content) const;
    Result<void> append(std::string_view relative, std::string_view line) const;
    Result<std::string> read_file(std::string_view relative) const;
    Result<void> lock(bool exclusive) const;
    void unlock() const;
    std::string path(std::string_view relative) const;

    std::string root_;
    std::string session_;
    mutable int lock_fd_ = -1;
};

} // namespace moonstone::mahak::coordination
