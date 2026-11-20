#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <optional>
#include <system_error>
#include <linux/kvm.h>

namespace domvirt {

struct GuestPhysAddr {
    uint64_t addr;
    explicit constexpr GuestPhysAddr(uint64_t a = 0) : addr(a) {}
    constexpr operator uint64_t() const { return addr; }
};

struct GuestVirtAddr {
    uint64_t addr;
    explicit constexpr GuestVirtAddr(uint64_t a = 0) : addr(a) {}
    constexpr operator uint64_t() const { return addr; }
};

struct MemRegionSpan {
    uint64_t gpa;
    uint64_t size;
    uint8_t* hva;
};

enum class VmExitReason : uint32_t {
    UNKNOWN       = KVM_EXIT_UNKNOWN,
    IO            = KVM_EXIT_IO,
    MMIO          = KVM_EXIT_MMIO,
    HLT           = KVM_EXIT_HLT,
    SHUTDOWN      = KVM_EXIT_SHUTDOWN,
    FAIL_ENTRY    = KVM_EXIT_FAIL_ENTRY,
    INTERNAL_ERROR = KVM_EXIT_INTERNAL_ERROR,
    HYPERCALL     = KVM_EXIT_HYPERCALL,
    IRQ_WINDOW_OPEN = KVM_EXIT_IRQ_WINDOW_OPEN,
};

enum class VmErrorCode : int {
    SUCCESS = 0,
    KVM_OPEN_FAILED,
    KVM_VERSION_MISMATCH,
    VM_CREATE_FAILED,
    VCPU_CREATE_FAILED,
    MEMORY_REGISTER_FAILED,
    MEMORY_ALLOC_FAILED,
    BOOT_LOAD_FAILED,
    ACPI_BUILD_FAILED,
    DEVICE_INIT_FAILED,
    INTERRUPT_ERROR,
    BRIDGE_ERROR,
    SNAPSHOT_ERROR,
    TIMEOUT,
    VIRTUAL_BACKEND_ERROR,
};

struct VmResult {
    VmErrorCode code{VmErrorCode::SUCCESS};
    std::string message;
    explicit operator bool() const { return code == VmErrorCode::SUCCESS; }
};

struct VcpuRegisters {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rsp, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer;
    uint64_t apic_base;
};

struct VirtioQueueConfig {
    uint16_t queue_size;
    uint64_t desc_addr;
    uint64_t avail_addr;
    uint64_t used_addr;
    uint32_t index;
};

struct AcpiTableHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

static constexpr size_t PAGE_SIZE = 4096;
static constexpr size_t PAGE_2MB = 0x200000;
static constexpr size_t PAGE_1GB = 0x40000000;

} // namespace domvirt
