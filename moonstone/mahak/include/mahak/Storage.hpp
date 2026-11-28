#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Json.hpp"
#include "Model.hpp"

namespace moonstone::mahak {
inline namespace v1 {

struct SessionSummary {
    std::string id;
    std::string manifest_id;
    std::string manifest_version;
    std::string status;
    std::uint64_t created_unix_ns = 0;
    std::uint64_t finished_unix_ns = 0;
    std::size_t run_count = 0;
    std::string host;
};

// Lossless JSON codec. decode() validates the schema and rejects unknown
// result-schema versions instead of silently dropping fields.
Result<Json> encode_session(const SessionRecord &session);
Result<SessionRecord> decode_session(const Json &json);
Result<std::string> encode_session_text(const SessionRecord &session, bool pretty = true);
Result<SessionRecord> decode_session_text(std::string_view text, std::string_view source = "<result>");
Result<void> validate_session(const SessionRecord &session);

// Explicit migration entry point. v0 denotes the pre-release flat run format
// documented in manual/05-storage.md. Unknown and future versions are errors.
Result<SessionRecord> migrate_session(const Json &json, std::uint32_t from_version);

// Interoperable tabular export: one row per raw sample plus a stable header.
Result<std::string> session_csv(const SessionRecord &session);
// A JSON Schema (draft 2020-12 subset) describing the v1 result document.
std::string result_schema_json();

// Persistent history store. Layout:
//   <root>/sessions/<id>.json    immutable committed session documents
//   <root>/index.json            rebuildable summary index
//   <root>/store.lock            advisory writer lock
// Commit writes a temporary file, fsyncs, renames atomically, then rewrites the
// index. Concurrent writers are serialized with flock. A crash leaves either
// the old or the new session, never a truncated JSON document.
class HistoryStore {
  public:
    static Result<HistoryStore> open(std::string root);
    ~HistoryStore();
    HistoryStore(HistoryStore &&) noexcept;
    HistoryStore &operator=(HistoryStore &&) noexcept;
    HistoryStore(const HistoryStore &) = delete;
    HistoryStore &operator=(const HistoryStore &) = delete;

    const std::string &root() const noexcept;
    // Committing an existing identical session is idempotent. A different
    // document with the same id is rejected.
    Result<void> commit(const SessionRecord &session);
    Result<std::vector<SessionSummary>> list() const;
    Result<SessionRecord> load(std::string_view id) const;
    Result<void> remove(std::string_view id);
    Result<void> recover();
    Result<void> rebuild_index();

  private:
    struct Impl;
    explicit HistoryStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Content hash used to detect duplicate/replaced session documents.
std::string session_digest(const SessionRecord &session);

} // namespace v1
} // namespace moonstone::mahak
