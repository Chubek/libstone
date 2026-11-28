#include "mahak/coordination/Blotter.hpp"
#include "mahak/coordination/Protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace moonstone::mahak::coordination {
namespace {

Error invalid(std::string message) {
    Error error;
    error.category = ErrorCategory::InvalidInput;
    error.message = std::move(message);
    return error;
}

Error internal(std::string message) {
    Error error;
    error.category = ErrorCategory::Internal;
    error.message = std::move(message);
    return error;
}

std::uint64_t now_unix_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool safe_token(std::string_view text, std::size_t maximum = 64) {
    if (text.empty() || text.size() > maximum)
        return false;
    for (char c : text) {
        const unsigned char value = static_cast<unsigned char>(c);
        const bool okay = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') || c == '-' || c == '_' ||
                          c == '.';
        if (!okay)
            return false;
    }
    return text != "." && text != "..";
}

bool safe_relative(std::string_view relative) {
    if (relative.empty() || relative.size() > 512 || relative.front() == '/')
        return false;
    std::string_view rest = relative;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return false;
        rest = slash == std::string_view::npos ? std::string_view{}
                                               : rest.substr(slash + 1);
    }
    return true;
}

bool write_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool fsync_directory(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool okay = ::fsync(fd) == 0;
    ::close(fd);
    return okay;
}

std::string read_whole_file(const std::string &path, bool &okay) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        okay = false;
        return {};
    }
    std::ostringstream output;
    output << stream.rdbuf();
    okay = stream.good() || stream.eof();
    return output.str();
}

} // namespace

Blotter::Blotter(std::string root, std::string session)
    : root_(std::move(root)), session_(std::move(session)) {}

Blotter::~Blotter() {
    if (lock_fd_ >= 0)
        ::close(lock_fd_);
}

Blotter::Blotter(Blotter &&other) noexcept
    : root_(std::move(other.root_)), session_(std::move(other.session_)),
      lock_fd_(other.lock_fd_) {
    other.lock_fd_ = -1;
}

Blotter &Blotter::operator=(Blotter &&other) noexcept {
    if (this != &other) {
        if (lock_fd_ >= 0)
            ::close(lock_fd_);
        root_ = std::move(other.root_);
        session_ = std::move(other.session_);
        lock_fd_ = other.lock_fd_;
        other.lock_fd_ = -1;
    }
    return *this;
}

const std::string &Blotter::root() const noexcept { return root_; }
const std::string &Blotter::session() const noexcept { return session_; }

std::string Blotter::path(std::string_view relative) const {
    return root_ + "/" + std::string(relative);
}

Result<void> Blotter::lock(bool exclusive) const {
    if (lock_fd_ < 0)
        return internal("blotter is not open");
    if (::flock(lock_fd_, exclusive ? LOCK_EX : LOCK_SH) == -1)
        return internal("flock failed: " + std::string(std::strerror(errno)));
    return {};
}

void Blotter::unlock() const {
    if (lock_fd_ >= 0)
        ::flock(lock_fd_, LOCK_UN);
}

Result<void> Blotter::initialize() {
    std::error_code error;
    if (!std::filesystem::create_directory(root_, error)) {
        if (error)
            return internal("cannot create blotter directory: " + error.message());
        return invalid("blotter session directory already exists: " + root_);
    }
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    for (const char *name : {"workers", "contracts", "verdicts", "spool"}) {
        if (!std::filesystem::create_directory(path(name), error) || error)
            return internal("cannot create blotter subdirectory: " + std::string(name) +
                            (error ? " (" + error.message() + ")" : ""));
        std::filesystem::permissions(path(name), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
    }
    for (const char *name : {"journal.ndjson", "decisions.ndjson"}) {
        const int fd = ::open(path(name).c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0)
            return internal("cannot create " + std::string(name) + ": " + std::strerror(errno));
        ::close(fd);
    }
    lock_fd_ = ::open(path("session.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0)
        return internal("cannot open blotter lock: " + std::string(std::strerror(errno)));
    Json manifest = Json::object();
    manifest.set("schema", 1);
    manifest.set("session", session_);
    manifest.set("root", root_);
    manifest.set("created_unix_ns", static_cast<unsigned long long>(now_unix_ns()));
    manifest.set("owner_pid", static_cast<long long>(::getpid()));
    manifest.set("revision", 0);
    manifest.set("workers", Json::array());
    auto write = write_atomic("manifest.json", manifest.dump(true) + "\n");
    if (!write)
        return write.error();
    // Worker records are created empty so a reader can enumerate the topology.
    return {};
}

Result<Blotter> Blotter::create(std::string session, std::vector<std::string> workers,
                                std::string parent) {
    if (!valid_session_id(session))
        return invalid("invalid session id");
    if (parent.empty())
        parent = "/dev/shm";
    while (parent.size() > 1 && parent.back() == '/')
        parent.pop_back();
    Blotter blotter(parent + "/mahak-" + session, session);
    auto initialized = blotter.initialize();
    if (!initialized)
        return initialized.error();
    Json names = Json::array();
    for (const auto &worker : workers) {
        if (!valid_worker_id(worker)) {
            blotter.cleanup();
            return invalid("invalid worker id: " + worker);
        }
        names.push(std::string(worker));
        Json record = Json::object();
        record.set("worker", worker);
        record.set("state", "registered");
        record.set("role", "");
        record.set("updated_unix_ns", static_cast<unsigned long long>(now_unix_ns()));
        record.set("revision", 0);
        auto written = blotter.write_atomic("workers/" + worker + ".json",
                                            record.dump(true) + "\n");
        if (!written) {
            blotter.cleanup();
            return written.error();
        }
    }
    Json manifest = blotter.manifest().value();
    Json updated = Json::object();
    updated.set("schema", 1);
    updated.set("session", session);
    updated.set("root", blotter.root());
    updated.set("created_unix_ns", manifest.find("created_unix_ns")
                                      ? *manifest.find("created_unix_ns")
                                      : Json(static_cast<unsigned long long>(now_unix_ns())));
    updated.set("owner_pid", static_cast<long long>(::getpid()));
    updated.set("revision", 0);
    updated.set("workers", names);
    auto rewritten = blotter.write_atomic("manifest.json", updated.dump(true) + "\n");
    if (!rewritten) {
        blotter.cleanup();
        return rewritten.error();
    }
    auto event = blotter.record_event("system", "session-created", updated);
    if (!event) {
        blotter.cleanup();
        return event.error();
    }
    return blotter;
}

Result<Blotter> Blotter::attach(std::string root) {
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    bool okay = false;
    const std::string text = read_whole_file(root + "/manifest.json", okay);
    if (!okay)
        return Error{ErrorCategory::NotFound, "blotter manifest not found: " + root, {}};
    auto parsed = Json::parse(text, root + "/manifest.json");
    if (!parsed)
        return parsed.error();
    const auto *session = parsed.value().find("session");
    if (!session || !session->as_string())
        return invalid("blotter manifest has no session");
    Blotter blotter(root, *session->as_string());
    blotter.lock_fd_ = ::open(blotter.path("session.lock").c_str(), O_RDWR | O_CLOEXEC);
    if (blotter.lock_fd_ < 0)
        return internal("cannot open blotter lock: " + std::string(std::strerror(errno)));
    return blotter;
}

Result<void> Blotter::write_atomic(std::string_view relative, std::string_view content) const {
    if (!safe_relative(relative))
        return invalid("unsafe blotter relative path");
    const std::string target = path(relative);
    std::string temporary = target + ".tmp.XXXXXX";
    std::vector<char> buffer(temporary.begin(), temporary.end());
    buffer.push_back('\0');
    const int fd = ::mkstemp(buffer.data());
    if (fd < 0)
        return internal("cannot create temporary blotter file: " + std::string(std::strerror(errno)));
    const std::string temporary_path(buffer.data());
    bool okay = ::fchmod(fd, 0600) == 0 && write_all(fd, content) && ::fsync(fd) == 0;
    if (::close(fd) != 0)
        okay = false;
    if (okay && ::rename(temporary_path.c_str(), target.c_str()) != 0)
        okay = false;
    if (okay) {
        const std::string directory = target.substr(0, target.find_last_of('/'));
        (void)fsync_directory(directory);
    }
    if (!okay) {
        const int saved = errno;
        ::unlink(temporary_path.c_str());
        return internal("atomic blotter write failed: " + std::string(std::strerror(saved)));
    }
    return {};
}

Result<void> Blotter::append(std::string_view relative, std::string_view line) const {
    if (!safe_relative(relative))
        return invalid("unsafe blotter relative path");
    const int fd = ::open(path(relative).c_str(), O_APPEND | O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return internal("cannot append journal: " + std::string(std::strerror(errno)));
    const std::string record = std::string(line) + "\n";
    const bool okay = write_all(fd, record) && ::fsync(fd) == 0;
    ::close(fd);
    if (!okay)
        return internal("journal append failed");
    return {};
}

Result<std::string> Blotter::read_file(std::string_view relative) const {
    if (!safe_relative(relative))
        return invalid("unsafe blotter relative path");
    bool okay = false;
    const std::string text = read_whole_file(path(relative), okay);
    if (!okay)
        return Error{ErrorCategory::NotFound, "blotter file not found: " + path(relative), {}};
    return text;
}

Result<Json> Blotter::manifest() const {
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    auto text = read_file("manifest.json");
    unlock();
    if (!text)
        return text.error();
    return Json::parse(text.value(), path("manifest.json"));
}

Result<std::uint64_t> Blotter::revision() const {
    auto records = read_journal();
    if (!records)
        return records.error();
    return static_cast<std::uint64_t>(records.value().size());
}

Result<std::vector<Json>> Blotter::read_journal() const {
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    auto text = read_file("journal.ndjson");
    unlock();
    if (!text)
        return text.error();
    std::vector<Json> events;
    std::istringstream stream(text.value());
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        if (line.empty())
            continue;
        auto parsed = Json::parse(line, "journal.ndjson:" + std::to_string(line_number));
        if (!parsed) {
            Error error = parsed.error();
            error.message = "corrupt journal line " + std::to_string(line_number) + ": " +
                            error.message;
            return error;
        }
        events.push_back(std::move(parsed.value()));
    }
    return events;
}

Result<std::uint64_t> Blotter::record_event(std::string_view actor, std::string_view kind,
                                            const Json &data) {
    auto locked = lock(true);
    if (!locked)
        return locked.error();
    std::uint64_t revision = 0;
    {
        bool okay = false;
        const std::string text = read_whole_file(path("journal.ndjson"), okay);
        if (okay) {
            revision = static_cast<std::uint64_t>(
                std::count(text.begin(), text.end(), '\n'));
        }
    }
    Json event = Json::object();
    event.set("revision", static_cast<unsigned long long>(revision + 1));
    event.set("timestamp_ns", static_cast<unsigned long long>(now_unix_ns()));
    event.set("actor", std::string(actor));
    event.set("kind", std::string(kind));
    event.set("data", data);
    auto appended = append("journal.ndjson", event.dump());
    if (!appended) {
        unlock();
        return appended.error();
    }
    // Update the manifest revision without losing any other field.
    bool okay = false;
    const std::string text = read_whole_file(path("manifest.json"), okay);
    if (okay) {
        auto parsed = Json::parse(text, path("manifest.json"));
        if (parsed) {
            Json manifest = parsed.value();
            manifest.set("revision", static_cast<unsigned long long>(revision + 1));
            (void)write_atomic("manifest.json", manifest.dump(true) + "\n");
        }
    }
    unlock();
    return revision + 1;
}

Result<void> Blotter::update_worker(std::string_view worker, const Json &status) {
    if (!safe_token(worker))
        return invalid("invalid worker id");
    auto locked = lock(true);
    if (!locked)
        return locked.error();
    Json record = status;
    if (!record.is_object())
        record = Json::object();
    record.set("worker", std::string(worker));
    record.set("updated_unix_ns", static_cast<unsigned long long>(now_unix_ns()));
    auto written = write_atomic("workers/" + std::string(worker) + ".json",
                                record.dump(true) + "\n");
    if (!written) {
        unlock();
        return written.error();
    }
    // Append a status event while still holding the lock. record_event() would
    // deadlock on a second exclusive flock in the same process.
    std::uint64_t revision = 0;
    {
        bool okay = false;
        const std::string text = read_whole_file(path("journal.ndjson"), okay);
        if (okay)
            revision = static_cast<std::uint64_t>(
                std::count(text.begin(), text.end(), '\n'));
    }
    Json event = Json::object();
    event.set("revision", static_cast<unsigned long long>(revision + 1));
    event.set("timestamp_ns", static_cast<unsigned long long>(now_unix_ns()));
    event.set("actor", std::string(worker));
    event.set("kind", "worker-status");
    event.set("data", record);
    auto appended = append("journal.ndjson", event.dump());
    if (appended) {
        bool okay = false;
        const std::string text = read_whole_file(path("manifest.json"), okay);
        if (okay) {
            auto parsed = Json::parse(text, path("manifest.json"));
            if (parsed) {
                Json manifest = parsed.value();
                manifest.set("revision", static_cast<unsigned long long>(revision + 1));
                (void)write_atomic("manifest.json", manifest.dump(true) + "\n");
            }
        }
    }
    unlock();
    return appended ? Result<void>{} : appended.error();
}

Result<Json> Blotter::worker(std::string_view worker) const {
    if (!safe_token(worker))
        return invalid("invalid worker id");
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    auto text = read_file("workers/" + std::string(worker) + ".json");
    unlock();
    if (!text)
        return text.error();
    return Json::parse(text.value(), path("workers/" + std::string(worker) + ".json"));
}

Result<std::vector<Json>> Blotter::workers() const {
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    std::vector<Json> records;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(path("workers"), error)) {
        if (error)
            break;
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        bool okay = false;
        const std::string text = read_whole_file(entry.path().string(), okay);
        if (!okay)
            continue;
        auto parsed = Json::parse(text, entry.path().string());
        if (parsed)
            records.push_back(std::move(parsed.value()));
    }
    unlock();
    if (error)
        return internal("cannot enumerate worker records: " + error.message());
    return records;
}

Result<std::uint32_t> Blotter::publish_contract(std::string_view name, const Json &contract) {
    if (!safe_token(name, 128))
        return invalid("invalid contract name");
    auto locked = lock(true);
    if (!locked)
        return locked.error();
    const std::string base = "contracts/" + std::string(name);
    bool okay = false;
    const std::string existing_text = read_whole_file(path(base + ".json"), okay);
    if (okay) {
        auto existing = Json::parse(existing_text, path(base + ".json"));
        if (existing && existing.value() == contract) {
            const auto *revision = existing.value().find("revision");
            unlock();
            return static_cast<std::uint32_t>(revision ? revision->as_uint64().value_or(1) : 1);
        }
    }
    std::uint32_t revision = 1;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(path("contracts"), error)) {
        if (error)
            break;
        const std::string filename = entry.path().filename().string();
        const std::string prefix = std::string(name) + ".r";
        if (filename.rfind(prefix, 0) == 0 &&
            entry.path().extension() == ".json") {
            const std::string number = filename.substr(prefix.size(),
                filename.size() - prefix.size() - 5);
            try {
                revision = std::max(revision,
                    static_cast<std::uint32_t>(std::stoul(number) + 1));
            } catch (...) {
            }
        }
    }
    Json document = contract;
    if (!document.is_object())
        document = Json::object();
    document.set("name", std::string(name));
    document.set("revision", static_cast<unsigned long long>(revision));
    document.set("published_unix_ns", static_cast<unsigned long long>(now_unix_ns()));
    auto revision_file = write_atomic(base + ".r" + std::to_string(revision) + ".json",
                                      document.dump(true) + "\n");
    auto latest_file = write_atomic(base + ".json", document.dump(true) + "\n");
    if (!revision_file || !latest_file) {
        unlock();
        return revision_file ? latest_file.error() : revision_file.error();
    }
    const std::uint64_t journal_revision = [&] {
        bool read_okay = false;
        const std::string text = read_whole_file(path("journal.ndjson"), read_okay);
        return read_okay ? static_cast<std::uint64_t>(std::count(text.begin(), text.end(), '\n')) + 1
                         : std::uint64_t{1};
    }();
    Json event = Json::object();
    event.set("revision", static_cast<unsigned long long>(journal_revision));
    event.set("timestamp_ns", static_cast<unsigned long long>(now_unix_ns()));
    event.set("actor", "contract");
    event.set("kind", "contract-published");
    event.set("data", document);
    (void)append("journal.ndjson", event.dump());
    unlock();
    return revision;
}

Result<Json> Blotter::contract(std::string_view name) const {
    if (!safe_token(name, 128))
        return invalid("invalid contract name");
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    auto text = read_file("contracts/" + std::string(name) + ".json");
    unlock();
    if (!text)
        return text.error();
    return Json::parse(text.value(), path("contracts/" + std::string(name) + ".json"));
}

Result<void> Blotter::record_decision(const Json &decision) {
    Json document = decision;
    if (!document.is_object())
        document = Json::object();
    document.set("timestamp_ns", static_cast<unsigned long long>(now_unix_ns()));
    auto locked = lock(true);
    if (!locked)
        return locked.error();
    auto appended = append("decisions.ndjson", document.dump());
    unlock();
    if (!appended)
        return appended.error();
    (void)record_event("orchestrator", "decision", document);
    return {};
}

Result<void> Blotter::record_verdict(std::string_view id, const Json &verdict) {
    if (!safe_token(id, 128))
        return invalid("invalid verdict id");
    Json document = verdict;
    if (!document.is_object())
        document = Json::object();
    document.set("id", std::string(id));
    document.set("recorded_unix_ns", static_cast<unsigned long long>(now_unix_ns()));
    auto locked = lock(true);
    if (!locked)
        return locked.error();
    const std::string relative = "verdicts/" + std::string(id) + ".json";
    bool okay = false;
    const std::string existing = read_whole_file(path(relative), okay);
    if (okay) {
        auto parsed = Json::parse(existing, path(relative));
        if (parsed && parsed.value() != document) {
            unlock();
            return invalid("verdict already recorded with different content: " +
                           std::string(id));
        }
        unlock();
        return {};
    }
    auto written = write_atomic(relative, document.dump(true) + "\n");
    unlock();
    if (!written)
        return written.error();
    (void)record_event("jury", "verdict", document);
    return {};
}

Result<Json> Blotter::verdict(std::string_view id) const {
    if (!safe_token(id, 128))
        return invalid("invalid verdict id");
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    auto text = read_file("verdicts/" + std::string(id) + ".json");
    unlock();
    if (!text)
        return text.error();
    return Json::parse(text.value(), path("verdicts/" + std::string(id) + ".json"));
}

Result<std::string> Blotter::spool_payload(const Json &payload) {
    static std::atomic<std::uint64_t> counter{0};
    const std::string name = "spool/" + make_session_id() + "-" +
                             std::to_string(++counter) + ".json";
    auto written = write_atomic(name, payload.dump() + "\n");
    if (!written)
        return written.error();
    return name;
}

Result<Json> Blotter::resolve_spool(std::string_view reference) const {
    if (reference.rfind("spool/", 0) != 0 || !safe_relative(reference))
        return invalid("invalid spool reference");
    auto text = read_file(reference);
    if (!text)
        return text.error();
    return Json::parse(text.value(), path(reference));
}

Result<std::vector<Json>> Blotter::read_decisions() const {
    auto locked = lock(false);
    if (!locked)
        return locked.error();
    auto text = read_file("decisions.ndjson");
    unlock();
    if (!text)
        return text.error();
    std::vector<Json> decisions;
    std::istringstream stream(text.value());
    std::string line;
    std::size_t number = 0;
    while (std::getline(stream, line)) {
        ++number;
        if (line.empty())
            continue;
        auto parsed = Json::parse(line, "decisions.ndjson:" + std::to_string(number));
        if (!parsed)
            return parsed.error();
        decisions.push_back(std::move(parsed.value()));
    }
    return decisions;
}

Result<void> Blotter::snapshot(std::string_view destination) const {
    if (destination.empty())
        return invalid("snapshot destination is empty");
    const std::string target(destination);
    std::error_code error;
    if (std::filesystem::exists(target, error))
        return invalid("snapshot destination already exists: " + target);
    if (!std::filesystem::create_directories(target, error) || error)
        return internal("cannot create snapshot directory: " + error.message());
    std::filesystem::permissions(target, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    std::filesystem::copy(root_, target,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::copy_symlinks,
                          error);
    if (error)
        return internal("snapshot copy failed: " + error.message());
    Json metadata = Json::object();
    metadata.set("schema", 1);
    metadata.set("source_root", root_);
    metadata.set("session", session_);
    const auto current_revision = revision();
    metadata.set("revision", static_cast<unsigned long long>(
                                current_revision ? current_revision.value() : 0));
    metadata.set("taken_unix_ns", static_cast<unsigned long long>(now_unix_ns()));
    std::ofstream out(target + "/SNAPSHOT.json", std::ios::binary | std::ios::trunc);
    if (!out)
        return internal("cannot write snapshot metadata");
    out << metadata.dump(true) << "\n";
    out.close();
    return {};
}

Result<void> Blotter::cleanup() {
    unlock();
    if (lock_fd_ >= 0) {
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
    if (root_.rfind("/dev/shm/mahak-", 0) != 0)
        return invalid("refusing to remove a non-/dev/shm/mahak-* path");
    bool okay = false;
    const std::string text = read_whole_file(root_ + "/manifest.json", okay);
    if (!okay)
        return Error{ErrorCategory::NotFound, "blotter manifest missing", {}};
    auto parsed = Json::parse(text, root_ + "/manifest.json");
    if (!parsed)
        return parsed.error();
    const auto *session = parsed.value().find("session");
    if (!session || !session->as_string() || *session->as_string() != session_)
        return invalid("blotter session manifest mismatch; refusing cleanup");
    std::error_code error;
    const auto status = std::filesystem::symlink_status(root_, error);
    if (error || std::filesystem::is_symlink(status))
        return invalid("refusing to remove a symlinked blotter path");
    std::filesystem::remove_all(root_, error);
    if (error)
        return internal("blotter cleanup failed: " + error.message());
    return {};
}

Result<Json> Blotter::inspect(std::string_view root) {
    auto attached = Blotter::attach(std::string(root));
    if (!attached)
        return attached.error();
    Blotter &blotter = attached.value();
    Json result = Json::object();
    auto manifest = blotter.manifest();
    if (!manifest)
        return manifest.error();
    result.set("manifest", manifest.value());
    auto workers = blotter.workers();
    if (!workers)
        return workers.error();
    result.set("workers", Json(workers.value()));
    auto journal = blotter.read_journal();
    if (!journal)
        return journal.error();
    result.set("journal_records", static_cast<unsigned long long>(journal.value().size()));
    auto decisions = blotter.read_decisions();
    if (!decisions)
        return decisions.error();
    result.set("decisions", static_cast<unsigned long long>(decisions.value().size()));
    return result;
}

} // namespace moonstone::mahak::coordination
