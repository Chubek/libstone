#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace firestone::memory {

using Handle = std::size_t;
struct Collection { std::size_t retained = 0, reclaimed = 0; };

// Explicit, synchronous tracing heap. Only edges supplied through allocate and
// set_references are traced; the payload T is opaque. Callers must supply all
// unpinned roots at collection time. Handles are stable until reclaimed (and
// never recycled within the heap); references returned by get() are not stable
// across allocations. Access and collection must not be concurrent or reentrant.
template<class T>
class GC {
public:
    GC() = default;
    GC(const GC&) = default;
    GC(GC&& other) noexcept { swap(other); }
    GC& operator=(const GC& other) {
        if (this != &other) {
            GC copy(other);
            swap(copy);
        }
        return *this;
    }
    GC& operator=(GC&& other) noexcept {
        if (this != &other) {
            GC moved(std::move(other));
            swap(moved);
        }
        return *this;
    }

    Handle allocate(T value, std::vector<Handle> references = {}) {
        check_references(references);
        slots_.emplace_back(Entry{std::move(value), std::move(references)});
        return slots_.size() - 1;
    }
    bool contains(Handle handle) const { return handle < slots_.size() && slots_[handle].has_value(); }
    T& get(Handle handle) { check(handle); return slots_[handle]->value; }
    const T& get(Handle handle) const { check(handle); return slots_[handle]->value; }
    const std::vector<Handle>& references(Handle handle) const { check(handle); return slots_[handle]->references; }
    void set_references(Handle handle, std::vector<Handle> references) {
        check(handle);
        check_references(references);
        // Record old-to-young writes before changing the graph: inserting
        // into the remembered set can throw.
        if (handle < young_begin_) {
            if (std::any_of(references.begin(), references.end(), [this](Handle child) { return child >= young_begin_; }))
                remembered_.insert(handle);
            else
                remembered_.erase(handle);
        }
        slots_[handle]->references = std::move(references);
    }
    void pin(Handle handle) { check(handle); pinned_.insert(handle); }
    void unpin(Handle handle) { check(handle); if (!pinned_.erase(handle)) throw std::logic_error("heap handle is not pinned"); }
    std::size_t size() const { return old_size_ + (slots_.size() - young_begin_); }

    // Full mark-and-sweep: reclaim every object not reachable from a root,
    // including unreachable cycles. All survivors become old.
    Collection collect(const std::vector<Handle>& roots = {}) {
        return collect_from(0, 0, roots);
    }

    // Generational minor collection: trace and sweep only allocations since
    // the last collection. Treat all old objects as live, including any that
    // became unreachable. Their remembered outgoing edges can keep young
    // objects alive; call collect() to recover that garbage. Promote every
    // survivor. As with collect(), retained counts all live heap entries.
    Collection collect_young(const std::vector<Handle>& roots = {}) {
        return collect_from(young_begin_, old_size_, roots);
    }
private:
    struct Entry { T value; std::vector<Handle> references; };
    std::vector<std::optional<Entry>> slots_;
    std::set<Handle> pinned_;
    std::set<Handle> remembered_; // Old entries with at least one young reference.
    // No collection leaves holes in the nursery: all its survivors are promoted.
    Handle young_begin_ = 0;
    std::size_t old_size_ = 0;

    Collection collect_from(Handle first, std::size_t retained, const std::vector<Handle>& roots) {
        check_references(roots);
        std::vector<bool> marked(slots_.size() - first);
        std::vector<Handle> pending;
        auto mark = [&](Handle handle) {
            if (handle >= first && !marked[handle - first]) {
                marked[handle - first] = true;
                pending.push_back(handle);
            }
        };
        for (auto it = pinned_.lower_bound(first); it != pinned_.end(); ++it) mark(*it);
        for (auto root : roots) mark(root);
        if (first != 0) {
            for (auto parent : remembered_)
                for (auto child : slots_[parent]->references) mark(child);
        }
        while (!pending.empty()) {
            auto handle = pending.back(); pending.pop_back();
            for (auto child : slots_[handle]->references) mark(child);
        }
        Collection result{retained, 0};
        for (Handle handle = first; handle < slots_.size(); ++handle) if (slots_[handle]) {
            if (marked[handle - first]) ++result.retained;
            else { slots_[handle].reset(); ++result.reclaimed; }
        }
        young_begin_ = slots_.size();
        old_size_ = result.retained;
        remembered_.clear();
        return result;
    }

    void swap(GC& other) noexcept {
        slots_.swap(other.slots_);
        pinned_.swap(other.pinned_);
        remembered_.swap(other.remembered_);
        std::swap(young_begin_, other.young_begin_);
        std::swap(old_size_, other.old_size_);
    }
    void check(Handle handle) const { if (!contains(handle)) throw std::out_of_range("invalid heap handle"); }
    void check_references(const std::vector<Handle>& references) const { for (auto handle : references) check(handle); }
};

} // namespace firestone::memory
