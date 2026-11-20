#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <sys/mman.h>
#include "domvirt_types.hpp"

namespace domvirt::hypervisor {

struct MemorySlot {
    uint32_t slot_id;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t host_userspace_addr;
    uint32_t flags;
    void* mmap_base{nullptr};
    size_t mmap_size{0};
};

class MemoryManager {
public:
    MemoryManager();
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;
    MemoryManager(MemoryManager&&) noexcept;
    MemoryManager& operator=(MemoryManager&&) noexcept;

    VmResult allocate_ram(uint64_t size, uint64_t guest_phys_base = 0,
                          bool use_hugepages = true, bool read_only = false);
    VmResult register_slot(int vm_fd, const MemorySlot& slot);
    VmResult register_all_slots(int vm_fd);
    VmResult enable_dirty_log(int vm_fd, uint32_t slot_id);
    VmResult get_dirty_log(int vm_fd, uint32_t slot_id);
    VmResult setup_identity_paging(uint64_t page_table_gpa = 0x9000);

    [[nodiscard]] uint8_t* translate(uint64_t gpa) const;
    [[nodiscard]] std::span<uint8_t> get_span(uint64_t gpa, size_t len) const;
    [[nodiscard]] const std::vector<MemorySlot>& slots() const noexcept { return slots_; }
    [[nodiscard]] size_t total_ram() const noexcept { return total_ram_; }

    void* host_ptr(uint64_t gpa) const;
    uint64_t gpa_from_host(void* hva) const;

private:
    std::vector<MemorySlot> slots_;
    size_t total_ram_{0};
};

} // namespace domvirt::hypervisor
