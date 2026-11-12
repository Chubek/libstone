#pragma once
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace moonstone::plaid::sentinel {
using ThreadId = unsigned;
using Address = std::size_t;
class VectorClock {
    std::map<ThreadId, std::size_t> clock_;
public:
    void tick(ThreadId id) { ++clock_[id]; }
    void join(const VectorClock& other) {
        for (const auto& [id,count] : other.clock_)
            if (clock_[id] < count) clock_[id] = count;
    }
    std::size_t get(ThreadId id) const { auto it=clock_.find(id); return it==clock_.end()?0:it->second; }
    bool ordered_before(const VectorClock& other) const {
        bool strictly = false;
        for (const auto& [id,n] : clock_) {
            if (n > other.get(id)) return false;
            if (n < other.get(id)) strictly = true;
        }
        for (const auto& [id,n] : other.clock_)
            if (!clock_.contains(id) && n > 0) strictly = true;
        return strictly;
    }
    bool concurrent_with(const VectorClock& other) const {
        return !ordered_before(other) && !other.ordered_before(*this);
    }
};
struct Access {
    ThreadId thread;
    Address location;
    bool write;
    bool atomic;
    VectorClock clock;
};
struct Race { Access first, second; };
// Offline trace checker. The caller must record all synchronization edges by
// joining participant clocks; this is not a runtime instrumenting race detector.
inline std::vector<Race> races(const std::vector<Access>& trace) {
    std::vector<Race> result;
    for (std::size_t i=0; i<trace.size(); ++i)
        for (std::size_t j=i+1; j<trace.size(); ++j) {
            const auto& a=trace[i]; const auto& b=trace[j];
            if (a.location==b.location && a.thread!=b.thread && (a.write||b.write)
                && (!a.atomic || !b.atomic) && a.clock.concurrent_with(b.clock))
                result.push_back({a,b});
        }
    return result;
}
class LockOrder {
    std::map<std::string,std::set<std::string>> edges_;
    bool reachable(const std::string& from, const std::string& target, std::set<std::string>& seen) const {
        if (from==target) return true;
        if (!seen.insert(from).second) return false;
        auto it=edges_.find(from);
        if (it==edges_.end()) return false;
        for (const auto& next:it->second) if (reachable(next,target,seen)) return true;
        return false;
    }
public:
    bool would_cycle(const std::string& held, const std::string& acquired) const {
        std::set<std::string> seen;
        return reachable(acquired,held,seen);
    }
    void add(const std::string& held, const std::string& acquired) {
        if (held.empty()||acquired.empty()) throw std::invalid_argument("empty lock name");
        if (would_cycle(held,acquired)) throw std::logic_error("lock-order cycle");
        edges_[held].insert(acquired);
    }
};
} // namespace moonstone::plaid::sentinel
