#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moonstone::prolific {

enum class Domain : std::uint8_t { Compiler, VM, Runtime };
enum class Kind : std::uint8_t { Begin, End, Instant, Counter, Error, Allocation, Free };
enum class Code { Ok, InvalidInput, Exhausted };
const char *name(Domain domain);
const char *name(Kind kind);

struct Location {
    std::string file, ir_id;
    std::uint32_t line = 0, column = 0; // zero means unavailable, not an invented span
};

struct Event {
    std::uint64_t sequence = 0, timestamp_ns = 0, thread = 0, span = 0, parent = 0;
    Domain domain = Domain::Runtime;
    Kind kind = Kind::Instant;
    std::string name, detail;
    Location location;
    std::int64_t value = 0;
    std::uint64_t duration_ns = 0, self_ns = 0;
};

struct Limits {
    // v1 hard limits: events <= 1,000,000; text <= 1 MiB. All limits nonzero.
    std::size_t events = 100000, bytes = 64 * 1024 * 1024, text = 4096;
    std::size_t threads = 256, depth = 256;
};

struct Snapshot {
    std::uint64_t created_unix_ns = 0, rejected = 0, open_spans = 0;
    std::vector<Event> events;
    bool complete() const { return rejected == 0 && open_spans == 0; }
};

struct BeginResult {
    Code code = Code::InvalidInput;
    std::uint64_t span = 0;
};

// Calls serialize on an internal mutex and may allocate/block an OS thread.
// No callbacks execute on producers. Not async-signal-safe. Destroy only after
// joining producers; nested spans cannot move between threads.
class Recorder {
  public:
    explicit Recorder(Limits limits = {});
    ~Recorder();
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    BeginResult begin(Domain domain, std::string_view label, Location location = {});
    Code end(std::uint64_t span);
    Code emit(Domain domain, Kind kind, std::string_view label, std::int64_t value = 0,
              std::string_view detail = {}, Location location = {});
    Snapshot snapshot() const;
    // Adapters must account for observations lost before recorder admission.
    void record_loss();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Lexical helper: recorder must outlive this object. A failed end is reflected
// in collection loss; destructors never throw. Scope is intentionally immovable.
class Scope {
  public:
    Scope(Recorder &recorder, Domain domain, std::string_view label, Location location = {});
    ~Scope() noexcept;
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    BeginResult result() const { return result_; }

  private:
    Recorder &recorder_;
    BeginResult result_;
};

struct Finding {
    std::string severity, code, message;
    std::uint64_t event_sequence = 0;
};

struct Report {
    Snapshot data;
    std::vector<Finding> findings;
    std::string analysis_status = "not_requested", analysis_error;
    std::string json() const;
};

// Strict, versioned cache wire format. Malformed/unsupported/oversized data
// throws std::runtime_error; no cache bytes are interpreted as native objects.
std::string encode(const Report &report);
Report decode(std::string_view bytes, std::size_t max_bytes = 128 * 1024 * 1024);

} // namespace moonstone::prolific
