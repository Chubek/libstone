#include "mahak/coordination/Roster.hpp"

#include "mahak/coordination/Protocol.hpp"

#include <cerrno>
#include <cstdint>
#include <optional>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace moonstone::mahak::coordination {
namespace {

using Object = Json::Object;

const Json *member(const Object &object, std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

Error roster_error(std::string message, ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::move(message);
    return error;
}

std::uint64_t unix_now_ns() {
    timespec value{};
    if (::clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(value.tv_nsec);
}

bool valid_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 32)
        return false;
    for (char character : name) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' ||
                          character == '_';
        if (!safe)
            return false;
    }
    return true;
}

Result<void> ensure_directory(std::string_view path) {
    if (::mkdir(std::string(path).c_str(), 0700) != 0 && errno != EEXIST)
        return roster_error("cannot create roster directory '" + std::string(path) +
                                "': " + std::strerror(errno),
                            ErrorCategory::Internal);
    struct stat status {};
    if (::stat(std::string(path).c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return roster_error("roster path is not a directory: " + std::string(path),
                            ErrorCategory::Internal);
    return {};
}

bool write_atomic(const std::string &path, const std::string &contents) {
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    std::size_t offset = 0;
    bool ok = true;
    while (offset < contents.size()) {
        const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written <= 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok && ::fsync(fd) != 0)
        ok = false;
    ::close(fd);
    if (!ok) {
        ::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    const std::size_t slash = path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
    const int handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (handle >= 0) {
        ::fsync(handle);
        ::close(handle);
    }
    return true;
}

Result<std::string> read_file(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return roster_error("missing file: " + path, ErrorCategory::NotFound);
        return roster_error("cannot open '" + path + "': " + std::strerror(errno),
                            ErrorCategory::Internal);
    }
    std::string contents;
    char buffer[4096];
    while (true) {
        const ssize_t bytes = ::read(fd, buffer, sizeof(buffer));
        if (bytes > 0) {
            contents.append(buffer, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes == 0)
            break;
        if (errno == EINTR)
            continue;
        ::close(fd);
        return roster_error("cannot read '" + path + "': " + std::strerror(errno),
                            ErrorCategory::Internal);
    }
    ::close(fd);
    return contents;
}

class FileLock {
  public:
    static Result<FileLock> acquire(const std::string &path) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0)
            return roster_error("cannot open lock '" + path + "': " + std::strerror(errno),
                                ErrorCategory::Internal);
        if (::flock(fd, LOCK_EX) != 0) {
            const int saved = errno;
            ::close(fd);
            return roster_error("cannot lock '" + path + "': " + std::strerror(saved),
                                ErrorCategory::Internal);
        }
        return FileLock(fd);
    }
    ~FileLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    FileLock(FileLock &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileLock &operator=(FileLock &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::flock(fd_, LOCK_UN);
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

  private:
    explicit FileLock(int fd) : fd_(fd) {}
    int fd_ = -1;
};

Result<Json> load_acks(std::string_view root) {
    const std::string path = std::string(root) + "/ACKS.json";
    auto text = read_file(path);
    if (!text) {
        if (text.error().category == ErrorCategory::NotFound)
            return Json::object();
        return text.error();
    }
    auto parsed = Json::parse(text.value(), path);
    if (!parsed)
        return parsed.error();
    if (!parsed.value().is_object())
        return roster_error("ACKS.json must be an object", ErrorCategory::InvalidInput);
    return parsed.value();
}

Result<RosterEntry> entry_from(const Json &value, std::size_t index) {
    if (!value.is_object())
        return roster_error("workers[" + std::to_string(index) + "] must be an object");
    const Object &object = *value.as_object();
    auto required = [&object](std::string_view key) -> Result<std::string> {
        const Json *field = member(object, key);
        const std::string *text = field != nullptr ? field->as_string() : nullptr;
        if (text == nullptr || text->empty())
            return roster_error("workers[" + std::string(key) + "] must be a non-empty string");
        return *text;
    };
    RosterEntry entry;
    auto name = required("name");
    if (!name)
        return name.error();
    entry.name = name.value();
    auto role = required("role");
    if (!role)
        return role.error();
    entry.role = role.value();
    auto inbox = required("inbox");
    if (!inbox)
        return inbox.error();
    entry.inbox = inbox.value();
    const Json *id = member(object, "protocol_id");
    const auto protocol_id = id != nullptr ? id->as_uint64() : std::nullopt;
    if (!protocol_id || *protocol_id == 0 || *protocol_id > 99)
        return roster_error("workers[" + entry.name + "].protocol_id must be 1..99");
    entry.protocol_id = static_cast<std::uint32_t>(*protocol_id);
    if (const Json *orchestrator = member(object, "orchestrator")) {
        const auto flag = orchestrator->as_bool();
        if (!flag)
            return roster_error("workers[" + entry.name + "].orchestrator must be a boolean");
        entry.orchestrator = *flag;
    }
    return entry;
}

} // namespace

std::string default_root() {
    return std::string(default_roster_root);
}

std::string roster_path(std::string_view root) {
    return std::string(root) + "/WORKERS.json";
}

const RosterEntry *Roster::lookup(std::string_view name) const {
    for (const auto &entry : entries) {
        if (entry.name == name)
            return &entry;
        if (std::to_string(entry.protocol_id) == std::string(name))
            return &entry;
    }
    return nullptr;
}

std::string Roster::display_name(std::string_view name) const {
    const RosterEntry *entry = lookup(name);
    return entry != nullptr ? entry->name : std::string();
}

Result<Roster> build_roster(std::string_view root, std::string_view session,
                            const std::vector<RosterIdentity> &identities) {
    if (!valid_session_id(session))
        return roster_error("invalid session id: " + std::string(session));
    if (identities.empty())
        return roster_error("roster must contain at least one worker");
    Roster roster;
    roster.root = std::string(root);
    roster.session = std::string(session);
    roster.published_unix_ns = unix_now_ns();
    std::uint32_t orchestrators = 0;
    for (const auto &identity : identities) {
        if (!valid_name(identity.name))
            return roster_error("invalid worker name: '" + identity.name + "'");
        if (identity.role.empty())
            return roster_error("worker '" + identity.name + "' has no role");
        if (identity.protocol_id == 0 || identity.protocol_id > 99)
            return roster_error("worker '" + identity.name + "' has an invalid protocol id");
        for (const auto &existing : roster.entries) {
            if (existing.name == identity.name)
                return roster_error("duplicate worker name: " + identity.name);
            if (existing.protocol_id == identity.protocol_id)
                return roster_error("duplicate protocol id: " +
                                    std::to_string(identity.protocol_id));
        }
        RosterEntry entry;
        entry.name = identity.name;
        entry.role = identity.role;
        entry.protocol_id = identity.protocol_id;
        entry.orchestrator = identity.orchestrator;
        const std::string suffix =
            identity.orchestrator ? std::string("broker")
                                  : "w" + std::to_string(identity.protocol_id);
        auto queue = mailbox_name(session, suffix);
        if (!queue)
            return queue.error();
        entry.inbox = queue.value();
        if (identity.orchestrator) {
            ++orchestrators;
            roster.broker_ingress = entry.inbox;
        }
        roster.entries.push_back(std::move(entry));
    }
    if (orchestrators != 1)
        return roster_error("roster must contain exactly one orchestrator");
    return roster;
}

Result<void> publish_roster(const Roster &roster) {
    if (roster.entries.empty())
        return roster_error("cannot publish an empty roster");
    if (roster.broker_ingress.empty())
        return roster_error("roster has no broker ingress");
    if (auto created = ensure_directory(roster.root); !created)
        return created.error();
    auto lock = FileLock::acquire(roster.root + "/roster.lock");
    if (!lock)
        return lock.error();

    Json document = Json::object();
    document.set("v", Json(1));
    document.set("revision", Json(static_cast<std::int64_t>(roster.revision)));
    document.set("session", Json(roster.session));
    document.set("root", Json(roster.root));
    document.set("broker_ingress", Json(roster.broker_ingress));
    document.set("published_unix_ns", Json(static_cast<std::int64_t>(roster.published_unix_ns)));
    Json workers = Json::array();
    for (const auto &entry : roster.entries) {
        Json item = Json::object();
        item.set("name", Json(entry.name));
        item.set("role", Json(entry.role));
        item.set("protocol_id", Json(static_cast<std::int64_t>(entry.protocol_id)));
        item.set("inbox", Json(entry.inbox));
        item.set("orchestrator", Json(entry.orchestrator));
        workers.push(std::move(item));
    }
    document.set("workers", std::move(workers));
    if (!write_atomic(roster_path(roster.root), document.dump(true)))
        return roster_error("cannot publish roster: " + std::string(std::strerror(errno)),
                            ErrorCategory::Internal);
    return {};
}

Result<Roster> load_roster(std::string_view root) {
    const std::string path = roster_path(root);
    auto text = read_file(path);
    if (!text)
        return text.error();
    auto parsed = Json::parse(text.value(), path);
    if (!parsed)
        return parsed.error();
    if (!parsed.value().is_object())
        return roster_error("WORKERS.json must be a JSON object");
    const Json &document = parsed.value();
    Roster roster;
    roster.root = std::string(root);
    const Json *revision = document.find("revision");
    const auto revision_value = revision != nullptr ? revision->as_uint64() : std::nullopt;
    if (!revision_value || *revision_value == 0)
        return roster_error("WORKERS.json has no valid revision");
    roster.revision = static_cast<std::uint32_t>(*revision_value);
    const Json *session = document.find("session");
    const std::string *session_text = session != nullptr ? session->as_string() : nullptr;
    if (session_text == nullptr || !valid_session_id(*session_text))
        return roster_error("WORKERS.json has no valid session id");
    roster.session = *session_text;
    const Json *ingress = document.find("broker_ingress");
    const std::string *ingress_text = ingress != nullptr ? ingress->as_string() : nullptr;
    if (ingress_text == nullptr || ingress_text->empty())
        return roster_error("WORKERS.json has no broker ingress");
    roster.broker_ingress = *ingress_text;
    if (const Json *published = document.find("published_unix_ns")) {
        if (const auto number = published->as_uint64())
            roster.published_unix_ns = *number;
    }
    const Json *workers = document.find("workers");
    const auto *array = workers != nullptr ? workers->as_array() : nullptr;
    if (array == nullptr || array->empty())
        return roster_error("WORKERS.json has no workers array");
    for (std::size_t index = 0; index < array->size(); ++index) {
        auto entry = entry_from((*array)[index], index);
        if (!entry)
            return entry.error();
        for (const auto &existing : roster.entries) {
            if (existing.name == entry.value().name)
                return roster_error("duplicate worker name: " + entry.value().name);
            if (existing.protocol_id == entry.value().protocol_id)
                return roster_error("duplicate protocol id in roster");
        }
        roster.entries.push_back(entry.value());
    }
    return roster;
}

Result<std::optional<Roster>> try_load_roster(std::string_view root) {
    auto loaded = load_roster(root);
    if (loaded)
        return std::optional<Roster>(std::move(loaded.value()));
    if (loaded.error().category == ErrorCategory::NotFound)
        return std::optional<Roster>();
    return loaded.error();
}

Result<RosterEntry> resolve_worker(const Roster &roster, std::string_view name) {
    const RosterEntry *entry = roster.lookup(name);
    if (entry == nullptr)
        return roster_error("unknown comrade '" + std::string(name) + "'", ErrorCategory::NotFound);
    return *entry;
}

Result<std::uint32_t> resolve_protocol_id(const Roster &roster, std::string_view name) {
    auto entry = resolve_worker(roster, name);
    if (!entry)
        return entry.error();
    return entry.value().protocol_id;
}

Result<void> acknowledge_roster(std::string_view root, std::string_view name,
                                std::uint32_t revision) {
    auto roster = load_roster(root);
    if (!roster)
        return roster.error();
    if (revision != roster.value().revision) {
        return roster_error("stale roster revision " + std::to_string(revision) +
                                "; current revision is " +
                                std::to_string(roster.value().revision),
                            ErrorCategory::InvalidInput);
    }
    auto entry = resolve_worker(roster.value(), name);
    if (!entry)
        return entry.error();
    if (auto created = ensure_directory(root); !created)
        return created.error();
    auto lock = FileLock::acquire(std::string(root) + "/roster.lock");
    if (!lock)
        return lock.error();

    Json acks = Json::object();
    auto existing = load_acks(root);
    if (!existing)
        return existing.error();
    const Json *current = existing.value().find("revision");
    const auto current_value = current != nullptr ? current->as_uint64() : std::nullopt;
    if (current_value && *current_value == revision) {
        if (const Json *acked = existing.value().find("acked")) {
            if (const auto *object = acked->as_object())
                acks = *object;
        }
    }
    acks.set(entry.value().name, Json(static_cast<std::int64_t>(unix_now_ns())));
    Json document = Json::object();
    document.set("v", Json(1));
    document.set("revision", Json(static_cast<std::int64_t>(revision)));
    document.set("acked", std::move(acks));
    if (!write_atomic(std::string(root) + "/ACKS.json", document.dump(true)))
        return roster_error("cannot update ACKS.json", ErrorCategory::Internal);
    return {};
}

Result<std::uint32_t> acknowledged_revision(std::string_view root) {
    auto acks = load_acks(root);
    if (!acks)
        return acks.error();
    const Json *revision = acks.value().find("revision");
    const auto value = revision != nullptr ? revision->as_uint64() : std::nullopt;
    return static_cast<std::uint32_t>(value ? *value : 0);
}

Result<std::vector<std::string>> pending_acknowledgements(const Roster &roster) {
    auto acks = load_acks(roster.root);
    if (!acks)
        return acks.error();
    const Json *revision = acks.value().find("revision");
    const auto value = revision != nullptr ? revision->as_uint64() : std::nullopt;
    const auto *acked = acks.value().find("acked");
    const Json::Object *object =
        value && *value == roster.revision && acked != nullptr ? acked->as_object() : nullptr;
    std::vector<std::string> pending;
    for (const auto &entry : roster.entries) {
        bool present = false;
        if (object != nullptr)
            present = object->find(entry.name) != object->end();
        if (!present)
            pending.push_back(entry.name);
    }
    return pending;
}

RosterLock::RosterLock(std::string root, int fd) : root_(std::move(root)), fd_(fd) {}

RosterLock::~RosterLock() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
}

RosterLock::RosterLock(RosterLock &&other) noexcept
    : root_(std::move(other.root_)), fd_(other.fd_) {
    other.fd_ = -1;
}

RosterLock &RosterLock::operator=(RosterLock &&other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
        root_ = std::move(other.root_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Result<RosterLock> RosterLock::acquire(std::string_view root) {
    if (auto created = ensure_directory(root); !created)
        return created.error();
    const std::string path = std::string(root) + "/session.lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return roster_error("cannot open session lock: " + std::string(std::strerror(errno)),
                            ErrorCategory::Internal);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno;
        ::close(fd);
        return roster_error(saved == EWOULDBLOCK
                                ? "roster directory is already owned by another session"
                                : std::string("cannot lock roster directory: ") +
                                      std::strerror(saved),
                            ErrorCategory::BudgetExhausted);
    }
    ::ftruncate(fd, 0);
    const std::string owner = std::to_string(::getpid()) + "\n";
    if (::write(fd, owner.data(), owner.size()) < 0) {
        // Ownership is still held; the pid record is diagnostic only.
    }
    return RosterLock(std::string(root), fd);
}

const std::string &RosterLock::root() const noexcept {
    return root_;
}

Result<long long> RosterLock::owner_pid() const {
    const std::string path = root_ + "/session.lock";
    auto text = read_file(path);
    if (!text)
        return text.error();
    try {
        return std::stoll(text.value());
    } catch (...) {
        return roster_error("session.lock does not contain a pid", ErrorCategory::InvalidInput);
    }
}

} // namespace moonstone::mahak::coordination
