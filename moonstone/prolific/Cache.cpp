#include "Cache.hpp"

#include <lmdb++.h>
#include <mutex>
#include <set>

namespace moonstone::prolific {
namespace {
std::mutex registry_mutex;
std::set<std::filesystem::path> registry;
// LMDB releases a transaction even when commit fails; the vendored wrapper
// only clears its handle on success. Disarm before the checked commit to avoid
// aborting freed memory on disk-full/I/O failures. Do not modify vendor code.
class WriteTransaction : public lmdb::txn {
  public:
    explicit WriteTransaction(MDB_env *env) : lmdb::txn(lmdb::txn::begin(env)) {}
    void commit() {
        auto *handle = _handle;
        _handle = nullptr;
        lmdb::txn_commit(handle);
    }
};
std::string run_key(std::string_view prefix, std::string_view run) {
    if (run.empty() || run.size() > 128)
        throw std::invalid_argument("Prolific run ID must have 1..128 ASCII characters");
    for (const auto c : run)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            throw std::invalid_argument("invalid Prolific run ID");
    return std::string(prefix) + std::string(run);
}
} // namespace

struct Cache::Impl {
    std::filesystem::path path;
    bool read_only;
    mutable std::mutex mutex;
    lmdb::env env = lmdb::env::create();

    Impl(const std::filesystem::path &directory, bool ro, std::size_t map_bytes) : read_only(ro) {
        if (directory.empty() || map_bytes < 64 * 1024)
            throw std::invalid_argument("Prolific requires a cache path and at least a 64 KiB map");
        if (!ro)
            std::filesystem::create_directories(directory);
        path = std::filesystem::canonical(directory);
        std::lock_guard lock(registry_mutex);
        if (!registry.insert(path).second)
            throw std::runtime_error("a Prolific cache is already open at " + path.string());
        try {
            env.set_mapsize(map_bytes);
            // NOTLS permits serialized readers to move between calling threads.
            // No NOSYNC/WRITEMAP flags: ordinary LMDB durability is retained.
            env.open(path.string().c_str(), MDB_NOTLS | (ro ? MDB_RDONLY : 0), 0600);
        } catch (...) {
            registry.erase(path);
            throw;
        }
    }
    ~Impl() {
        std::lock_guard lock(registry_mutex);
        env.close();
        registry.erase(path);
    }
    std::optional<std::string> get(const std::string &key) const {
        std::lock_guard lock(mutex);
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto db = lmdb::dbi::open(txn);
        const lmdb::val k{key};
        lmdb::val value{};
        if (!db.get(txn, k, value))
            return std::nullopt;
        if (value.size() > 128 * 1024 * 1024)
            throw std::runtime_error("Prolific cached value exceeds the 128 MiB read limit");
        return std::string(value.data(), value.size());
    }
};

Cache::Cache(const std::filesystem::path &directory, bool ro, std::size_t bytes)
    : impl_(std::make_unique<Impl>(directory, ro, bytes)) {}
Cache::~Cache() = default;

void Cache::save(std::string_view run, const Report &report, bool replace) {
    const auto snapshot_key = run_key("snapshot/", run), json_key = run_key("report/", run);
    if (impl_->read_only)
        throw std::runtime_error("Prolific cache is read-only");
    const auto bytes = encode(report), json = report.json();
    if (json.size() > 128 * 1024 * 1024)
        throw std::runtime_error("Prolific JSON exceeds the 128 MiB cache value limit");
    std::lock_guard lock(impl_->mutex);
    WriteTransaction txn(impl_->env);
    auto db = lmdb::dbi::open(txn);
    const lmdb::val snapshot_k{snapshot_key}, json_k{json_key};
    lmdb::val snapshot_v{bytes}, json_v{json};
    // Use explicit byte views: lmdbxx's generic templates store object
    // representations, not the characters of a std::string.
    if (!db.put(txn, snapshot_k, snapshot_v, replace ? 0 : MDB_NOOVERWRITE))
        throw std::runtime_error("Prolific run already exists: " + std::string(run));
    db.put(txn, json_k, json_v);
    txn.commit();
}

std::optional<Report> Cache::load(std::string_view run) const {
    auto bytes = impl_->get(run_key("snapshot/", run));
    if (!bytes)
        return std::nullopt;
    return decode(*bytes);
}
std::optional<std::string> Cache::json(std::string_view run) const {
    return impl_->get(run_key("report/", run));
}
std::vector<std::string> Cache::runs() const {
    std::lock_guard lock(impl_->mutex);
    auto txn = lmdb::txn::begin(impl_->env, nullptr, MDB_RDONLY);
    auto db = lmdb::dbi::open(txn);
    auto cursor = lmdb::cursor::open(txn, db);
    lmdb::val key{}, value{};
    std::vector<std::string> result;
    while (cursor.get(key, value, MDB_NEXT)) {
        const std::string_view view(key.data(), key.size());
        if (view.starts_with("snapshot/")) {
            (void)run_key("snapshot/", view.substr(9));
            result.emplace_back(view.substr(9));
        }
    }
    return result;
}
} // namespace moonstone::prolific
