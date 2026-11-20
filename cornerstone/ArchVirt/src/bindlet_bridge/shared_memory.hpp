#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <mutex>
#include "domvirt_types.hpp"
struct _object;

namespace domvirt::bindlet_bridge {

using PyObject = _object;

struct SharedMemorySlot {
    uint64_t gpa_base;
    size_t size;
    uint8_t* hva;
    PyObject* py_buffer{nullptr};
    bool dirty{false};
};

class SharedMemoryView {
public:
    SharedMemoryView() = default;
    ~SharedMemoryView();

    SharedMemoryView(const SharedMemoryView&) = delete;
    SharedMemoryView& operator=(const SharedMemoryView&) = delete;

    void register_region(uint64_t gpa, size_t size, uint8_t* hva);
    void unregister_all();

    PyObject* get_python_view(uint64_t gpa, size_t size);

    std::span<uint8_t> get_span(uint64_t gpa, size_t size);
    std::span<const uint8_t> get_span(uint64_t gpa, size_t size) const;

    void mark_dirty(uint64_t gpa, size_t size);
    bool is_dirty(uint64_t gpa) const;
    void sync_dirty();

private:
    SharedMemorySlot* find_slot(uint64_t gpa);
    const SharedMemorySlot* find_slot(uint64_t gpa) const;

    std::vector<SharedMemorySlot> slots_;
    mutable std::mutex mutex_;
};

class GuestBufferManager {
public:
    GuestBufferManager() = default;
    ~GuestBufferManager();

    GuestBufferManager(const GuestBufferManager&) = delete;
    GuestBufferManager& operator=(const GuestBufferManager&) = delete;

    uint8_t* allocate_guest_buffer(size_t size, uint64_t gpa);
    void free_guest_buffer(uint64_t gpa);
    bool validate_access(uint64_t gpa, size_t size) const;
    void register_memory_slot(uint64_t gpa, size_t size, uint8_t* hva);

private:
    struct BufferEntry {
        uint64_t gpa;
        size_t size;
        uint8_t* hva;
        bool owned;
    };
    std::vector<BufferEntry> buffers_;
    mutable std::mutex mutex_;
};

} // namespace domvirt::bindlet_bridge
