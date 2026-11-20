#include "shared_memory.hpp"
#include <Python.h>
#include <cstring>
#include <algorithm>
#include <sys/mman.h>
#include <cstdlib>
#include <limits>

namespace {
bool range_contains(uint64_t base, size_t length, uint64_t address, size_t size) {
    if (address < base || size > std::numeric_limits<uint64_t>::max() - address) return false;
    const uint64_t end = address + size;
    return end >= address && end <= base + length && base + length >= base;
}
}

namespace domvirt::bindlet_bridge {

SharedMemoryView::~SharedMemoryView() {
    unregister_all();
}

void SharedMemoryView::register_region(uint64_t gpa, size_t size, uint8_t* hva) {
    std::lock_guard<std::mutex> lock(mutex_);
    SharedMemorySlot slot{gpa, size, hva, nullptr, false};
    slots_.push_back(slot);
}

void SharedMemoryView::unregister_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (slot.py_buffer) {
            Py_DECREF(slot.py_buffer);
            slot.py_buffer = nullptr;
        }
    }
    slots_.clear();
}

PyObject* SharedMemoryView::get_python_view(uint64_t gpa, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (range_contains(slot.gpa_base, slot.size, gpa, size)) {
            if (!slot.py_buffer) {
                slot.py_buffer = PyMemoryView_FromMemory(
                    reinterpret_cast<char*>(slot.hva),
                    static_cast<Py_ssize_t>(slot.size), PyBUF_READ);
            }
            Py_XINCREF(slot.py_buffer);
            return slot.py_buffer;
        }
    }
    return nullptr;
}

std::span<uint8_t> SharedMemoryView::get_span(uint64_t gpa, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (range_contains(slot.gpa_base, slot.size, gpa, size)) {
            return std::span<uint8_t>(slot.hva + (gpa - slot.gpa_base), size);
        }
    }
    return {};
}

std::span<const uint8_t> SharedMemoryView::get_span(uint64_t gpa, size_t size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (range_contains(slot.gpa_base, slot.size, gpa, size)) {
            return std::span<const uint8_t>(
                slot.hva + (gpa - slot.gpa_base), size);
        }
    }
    return {};
}

void SharedMemoryView::mark_dirty(uint64_t gpa, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (range_contains(slot.gpa_base, slot.size, gpa, size)) {
            slot.dirty = true;
            return;
        }
    }
}

bool SharedMemoryView::is_dirty(uint64_t gpa) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (gpa >= slot.gpa_base && gpa < slot.gpa_base + slot.size) {
            return slot.dirty;
        }
    }
    return false;
}

void SharedMemoryView::sync_dirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        if (slot.dirty) {
            msync(slot.hva, slot.size, MS_SYNC);
            slot.dirty = false;
        }
    }
}

SharedMemorySlot* SharedMemoryView::find_slot(uint64_t gpa) {
    for (auto& slot : slots_) {
        if (gpa >= slot.gpa_base && gpa < slot.gpa_base + slot.size) {
            return &slot;
        }
    }
    return nullptr;
}

const SharedMemorySlot* SharedMemoryView::find_slot(uint64_t gpa) const {
    for (auto& slot : slots_) {
        if (gpa >= slot.gpa_base && gpa < slot.gpa_base + slot.size) {
            return &slot;
        }
    }
    return nullptr;
}

GuestBufferManager::~GuestBufferManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : buffers_) {
        if (entry.owned && entry.hva) {
            free(entry.hva);
        }
    }
    buffers_.clear();
}

uint8_t* GuestBufferManager::allocate_guest_buffer(size_t size, uint64_t gpa) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size == 0) return nullptr;
    const size_t allocation_size =
        (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint8_t* buf = static_cast<uint8_t*>(
        aligned_alloc(PAGE_SIZE, allocation_size));
    if (!buf) return nullptr;
    buffers_.push_back({gpa, allocation_size, buf, true});
    return buf;
}

void GuestBufferManager::free_guest_buffer(uint64_t gpa) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->gpa == gpa) {
            if (it->owned && it->hva) free(it->hva);
            buffers_.erase(it);
            return;
        }
    }
}

bool GuestBufferManager::validate_access(uint64_t gpa, size_t size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : buffers_) {
        if (range_contains(entry.gpa, entry.size, gpa, size)) {
            return true;
        }
    }
    return false;
}

void GuestBufferManager::register_memory_slot(uint64_t gpa, size_t size, uint8_t* hva) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.push_back({gpa, size, hva, false});
}

} // namespace domvirt::bindlet_bridge
