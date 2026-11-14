#include <exotract/Exotract.hpp>
#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>
#if defined(EXOTRACT_WITH_LMDB)
#include <lmdb++.h>
#endif
namespace moonstone::exotract {
#if defined(EXOTRACT_WITH_LMDB)
namespace {
std::mutex environment_mutex;
std::set<fs::path> environments;
}
class WriteTransaction : public lmdb::txn {
public:
    explicit WriteTransaction(MDB_env *env) : lmdb::txn(lmdb::txn::begin(env)) {}
    void commit_checked() { auto *handle = _handle; _handle = nullptr; lmdb::txn_commit(handle); }
};
#endif
struct Store::Impl {
    fs::path path;
    mutable std::mutex lock;
    mutable std::unordered_map<std::string, std::string> memory;
#if defined(EXOTRACT_WITH_LMDB)
    lmdb::env env = lmdb::env::create();
    explicit Impl(fs::path p) : path(std::move(p)) {
        if (!path.empty()) {
            fs::create_directories(path);
            path=fs::canonical(path);
            std::lock_guard guard(environment_mutex);
            if(!environments.insert(path).second)throw std::runtime_error("LMDB environment already open in this process; share a Store");
            try {
                env.set_mapsize(256u * 1024u * 1024u);
                env.open(path.string().c_str(), MDB_NOTLS, 0600);
            } catch (...) { environments.erase(path); throw; }
        }
    }
    ~Impl() {
        if(!path.empty()){
            std::lock_guard guard(environment_mutex);
            env.close();environments.erase(path);
        }
    }
#else
    explicit Impl(fs::path p) : path(std::move(p)) {
        if (!path.empty()) throw std::runtime_error("persistent Exotract cache requires LMDB; rebuild with EXOTRACT_WITH_LMDB");
    }
#endif
};
Store::Store(fs::path path) : impl_(std::make_unique<Impl>(std::move(path))) {}
Store::~Store() = default;
bool Store::persistent() const { return !impl_->path.empty(); }
std::optional<std::string> Store::get(std::string_view key) const {
    if (key.empty() || key.size() > 511) throw std::invalid_argument("invalid cache key");
    std::lock_guard lock(impl_->lock);
#if defined(EXOTRACT_WITH_LMDB)
    if (persistent()) {
        auto txn = lmdb::txn::begin(impl_->env, nullptr, MDB_RDONLY);
        auto db = lmdb::dbi::open(txn);
        const lmdb::val k{key.data(), key.size()}; lmdb::val v{};
        if (!db.get(txn, k, v)) return std::nullopt;
        if (v.size() > 128u * 1024u * 1024u) throw std::runtime_error("oversized LMDB value");
        return std::string(v.data(), v.size());
    }
#endif
    auto it=impl_->memory.find(std::string(key));
    return it == impl_->memory.end() ? std::nullopt : std::optional<std::string>(it->second);
}
void Store::put(std::string_view key, std::string_view value) {
    if (key.empty() || key.size() > 511 || value.size() > 128u*1024u*1024u)
        throw std::invalid_argument("cache key/value exceeds budget");
    std::lock_guard lock(impl_->lock);
#if defined(EXOTRACT_WITH_LMDB)
    if (persistent()) {
        WriteTransaction txn(impl_->env);
        auto db = lmdb::dbi::open(txn);
        lmdb::val k{key.data(), key.size()}, v{value.data(), value.size()};
        db.put(txn, k, v);
        // The vendored lmdbxx txn wrapper may retain a freed handle if commit
        // fails; detach before committing, mirroring Prolific's implementation.
        txn.commit_checked();
        return;
    }
#endif
    std::size_t total=key.size()+value.size();
    for(const auto &[k,v]:impl_->memory) if(k!=key) total+=k.size()+v.size();
    if(total>128u*1024u*1024u)throw std::length_error("in-memory store byte budget");
    impl_->memory[std::string(key)] = std::string(value);
}
GlobalRegistry::GlobalRegistry(std::shared_ptr<Store> store):store_(std::move(store)){
    if (!store_) throw std::invalid_argument("missing GlobalRegistry store");
}
namespace {
void name_check(std::string_view n) {
    if(n.empty() || n.size()>128 || !((n[0]>='A'&&n[0]<='Z')||n[0]=='_'))
        throw std::invalid_argument("invalid GLOBAL identifier");
    for(char c:n) if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))
        throw std::invalid_argument("invalid GLOBAL identifier");
}
}
void GlobalRegistry::set(std::string name,std::string value) {
    name_check(name);
    if(value.find('\0')!=std::string::npos || value.size()>64*1024)
        throw std::invalid_argument("invalid GLOBAL value");
    store_->put("global/"+name,value);
}
std::optional<std::string> GlobalRegistry::get(std::string_view name) const {
    name_check(name);
    return store_->get("global/"+std::string(name));
}
std::string GlobalRegistry::expand(std::string_view source) const {
    std::string out;out.reserve(source.size());
    std::size_t i=0;
    while(i<source.size()) {
        auto p=source.find("$GLOBAL{",i);
        if(p==std::string_view::npos){out.append(source.substr(i));break;}
        out.append(source.substr(i,p-i));
        auto end=source.find('}',p+8);
        if(end==std::string_view::npos)throw std::invalid_argument("unterminated $GLOBAL expansion");
        auto name=source.substr(p+8,end-(p+8));
        auto val=get(name);
        if(!val)throw std::runtime_error("undefined GLOBAL: "+std::string(name));
        if(out.size()+val->size()>1024*1024)throw std::length_error("expanded path too long");
        out+=*val;i=end+1;
    }
    return out;
}
} // namespace moonstone::exotract
