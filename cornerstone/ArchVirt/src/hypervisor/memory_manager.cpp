#include "memory_manager.hpp"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <memory>
#include <limits>

namespace domvirt::hypervisor {

MemoryManager::MemoryManager() = default;

MemoryManager::~MemoryManager() {
    for (auto& slot : slots_) {
        if (slot.mmap_base) {
            munmap(slot.mmap_base, slot.mmap_size);
        }
    }
}

MemoryManager::MemoryManager(MemoryManager&& other) noexcept
    : slots_(std::move(other.slots_))
    , total_ram_(other.total_ram_) {
    other.total_ram_ = 0;
}

MemoryManager& MemoryManager::operator=(MemoryManager&& other) noexcept {
    if (this != &other) {
        for (auto& slot : slots_) {
            if (slot.mmap_base) munmap(slot.mmap_base, slot.mmap_size);
        }
        slots_ = std::move(other.slots_);
        total_ram_ = other.total_ram_;
        other.total_ram_ = 0;
    }
    return *this;
}

VmResult MemoryManager::allocate_ram(uint64_t size, uint64_t guest_phys_base,
                                     bool use_hugepages, bool read_only) {
    if (size == 0) {
        return {VmErrorCode::MEMORY_ALLOC_FAILED, "Memory size must be positive"};
    }

    if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        guest_phys_base > std::numeric_limits<uint64_t>::max() - size) {
        return {VmErrorCode::MEMORY_ALLOC_FAILED, "Requested memory range overflows"};
    }
    const uint64_t requested_end = guest_phys_base + size;
    for (const auto& existing : slots_) {
        const uint64_t existing_end = existing.guest_phys_addr + existing.memory_size;
        if (guest_phys_base < existing_end && existing.guest_phys_addr < requested_end) {
            return {VmErrorCode::MEMORY_ALLOC_FAILED, "Requested memory range overlaps an existing slot"};
        }
    }
    size_t alloc_size = static_cast<size_t>(size);
    int flags = MAP_ANONYMOUS | MAP_PRIVATE;

    if (use_hugepages) {
        if (alloc_size >= PAGE_1GB) {
            alloc_size = (alloc_size + PAGE_1GB - 1) & ~(PAGE_1GB - 1);
            flags |= MAP_HUGETLB | (30 << MAP_HUGE_SHIFT) | MAP_POPULATE;
        } else if (alloc_size >= PAGE_2MB) {
            alloc_size = (alloc_size + PAGE_2MB - 1) & ~(PAGE_2MB - 1);
            flags |= MAP_HUGETLB | (21 << MAP_HUGE_SHIFT) | MAP_POPULATE;
        }
    } else {
        alloc_size = (alloc_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        flags |= MAP_NORESERVE;
    }

    void* ptr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (ptr == MAP_FAILED) {
        alloc_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        ptr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    }
    if (ptr == MAP_FAILED) {
        return {VmErrorCode::MEMORY_ALLOC_FAILED,
                "mmap failed: " + std::string(strerror(errno))};
    }

    MemorySlot slot{};
    slot.slot_id = static_cast<uint32_t>(slots_.size());
    slot.guest_phys_addr = guest_phys_base;
    slot.memory_size = alloc_size;
    slot.host_userspace_addr = reinterpret_cast<uint64_t>(ptr);
    slot.mmap_base = ptr;
    slot.mmap_size = alloc_size;
    slot.flags = read_only ? KVM_MEM_READONLY : 0;

    slots_.push_back(slot);
    total_ram_ += alloc_size;

    return {VmErrorCode::SUCCESS, "Allocated " + std::to_string(alloc_size) + " bytes"};
}

VmResult MemoryManager::register_slot(int vm_fd, const MemorySlot& slot) {
    if (slot.memory_size == 0 || slot.mmap_base == nullptr ||
        slot.guest_phys_addr > std::numeric_limits<uint64_t>::max() - slot.memory_size) {
        return {VmErrorCode::MEMORY_REGISTER_FAILED, "Invalid memory slot"};
    }
    struct kvm_userspace_memory_region region{};
    region.slot = slot.slot_id;
    region.flags = slot.flags;
    region.guest_phys_addr = slot.guest_phys_addr;
    region.memory_size = slot.memory_size;
    region.userspace_addr = slot.host_userspace_addr;

    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        return {VmErrorCode::MEMORY_REGISTER_FAILED,
                "KVM_SET_USER_MEMORY_REGION failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, "Memory slot registered"};
}

VmResult MemoryManager::register_all_slots(int vm_fd) {
    for (auto& slot : slots_) {
        auto result = register_slot(vm_fd, slot);
        if (!result) return result;
    }
    return {VmErrorCode::SUCCESS, "All memory slots registered"};
}

VmResult MemoryManager::enable_dirty_log(int vm_fd, uint32_t slot_id) {
    for (auto& slot : slots_) {
        if (slot.slot_id == slot_id) {
            slot.flags |= KVM_MEM_LOG_DIRTY_PAGES;
            return register_slot(vm_fd, slot);
        }
    }
    return {VmErrorCode::MEMORY_REGISTER_FAILED, "Slot not found: " + std::to_string(slot_id)};
}

VmResult MemoryManager::get_dirty_log(int vm_fd, uint32_t slot_id) {
    for (auto& slot : slots_) {
        if (slot.slot_id == slot_id) {
            size_t bitmap_size = (slot.memory_size / PAGE_SIZE + 7) / 8;
            auto bitmap = std::make_unique<uint8_t[]>(bitmap_size);
            memset(bitmap.get(), 0, bitmap_size);

            struct kvm_dirty_log log{};
            log.slot = slot_id;
            log.dirty_bitmap = bitmap.get();

            if (ioctl(vm_fd, KVM_GET_DIRTY_LOG, &log) < 0) {
                return {VmErrorCode::MEMORY_REGISTER_FAILED,
                        "KVM_GET_DIRTY_LOG failed: " + std::string(strerror(errno))};
            }
            return {VmErrorCode::SUCCESS, "Dirty log retrieved"};
        }
    }
    return {VmErrorCode::MEMORY_REGISTER_FAILED, "Slot not found: " + std::to_string(slot_id)};
}

VmResult MemoryManager::setup_identity_paging(uint64_t page_table_gpa) {
    constexpr uint64_t present = 1;
    constexpr uint64_t writable = 1ull << 1;
    constexpr uint64_t huge = 1ull << 7;
    constexpr uint64_t page_size = 0x1000;
    const uint64_t pdpt_gpa = page_table_gpa + page_size;
    const uint64_t pd_gpa = page_table_gpa + 2 * page_size;
    auto* pml4 = reinterpret_cast<uint64_t*>(translate(page_table_gpa));
    auto* pdpt = reinterpret_cast<uint64_t*>(translate(pdpt_gpa));
    auto* pd = reinterpret_cast<uint64_t*>(translate(pd_gpa));
    if (!pml4 || !pdpt || !pd || page_table_gpa > total_ram_ ||
        3 * page_size > total_ram_ - page_table_gpa) {
        return {VmErrorCode::MEMORY_ALLOC_FAILED, "Identity page tables do not fit guest memory"};
    }
    std::memset(pml4, 0, page_size);
    std::memset(pdpt, 0, page_size);
    std::memset(pd, 0, page_size);
    pml4[0] = pdpt_gpa | present | writable;
    pdpt[0] = pd_gpa | present | writable;
    constexpr std::size_t entries = 512;
    for (std::size_t i = 0; i < entries; ++i) {
        const uint64_t address = static_cast<uint64_t>(i) * 0x200000ull;
        pd[i] = address | present | writable | huge;
    }
    return {VmErrorCode::SUCCESS, "Identity paging initialized"};
}

uint8_t* MemoryManager::translate(uint64_t gpa) const {
    for (auto& slot : slots_) {
        if (gpa >= slot.guest_phys_addr &&
            gpa < slot.guest_phys_addr + slot.memory_size) {
            return static_cast<uint8_t*>(slot.mmap_base) +
                   (gpa - slot.guest_phys_addr);
        }
    }
    return nullptr;
}

std::span<uint8_t> MemoryManager::get_span(uint64_t gpa, size_t len) const {
    if (len > std::numeric_limits<uint64_t>::max() - gpa) return {};
    for (auto& slot : slots_) {
        if (gpa >= slot.guest_phys_addr &&
            gpa + len <= slot.guest_phys_addr + slot.memory_size) {
            return std::span<uint8_t>(
                static_cast<uint8_t*>(slot.mmap_base) +
                (gpa - slot.guest_phys_addr), len);
        }
    }
    return {};
}

void* MemoryManager::host_ptr(uint64_t gpa) const {
    return translate(gpa);
}

uint64_t MemoryManager::gpa_from_host(void* hva) const {
    uint64_t hva_addr = reinterpret_cast<uint64_t>(hva);
    for (auto& slot : slots_) {
        if (slot.host_userspace_addr <= std::numeric_limits<uint64_t>::max() - slot.memory_size &&
            hva_addr >= slot.host_userspace_addr &&
            hva_addr < slot.host_userspace_addr + slot.memory_size) {
            return slot.guest_phys_addr +
                   (hva_addr - slot.host_userspace_addr);
        }
    }
    return 0;
}

} // namespace domvirt::hypervisor
