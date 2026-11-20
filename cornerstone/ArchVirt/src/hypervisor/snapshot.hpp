#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include "domvirt_types.hpp"
#include "memory_manager.hpp"

namespace domvirt::hypervisor {

struct SnapshotHeader {
    char magic[8] = {'D','O','M','S','N','A','P',0};
    uint32_t version{1};
    uint64_t timestamp;
    uint64_t memory_size;
    uint32_t num_vcpus;
    uint32_t num_dirty_pages;
    uint64_t dirty_bitmap_offset;
    uint64_t memory_offset;
    uint64_t vcpu_state_offset;
    uint32_t checksum;
};

struct VcpuSnapshot {
    int vcpu_id;
    VcpuRegisters regs;
    struct kvm_sregs sregs;
    struct kvm_fpu fpu;
    struct kvm_lapic_state lapic;
    struct kvm_mp_state mp_state;
    struct kvm_vcpu_events events;
};

class SnapshotEngine {
public:
    SnapshotEngine() = default;
    ~SnapshotEngine() = default;

    VmResult create_full_snapshot(int vm_fd, MemoryManager& memory,
                                  const std::vector<VcpuSnapshot>& vcpu_states,
                                  const std::string& output_path);

    VmResult create_incremental_snapshot(int vm_fd, MemoryManager& memory,
                                         const std::vector<VcpuSnapshot>& vcpu_states,
                                         const std::string& output_path);

    VmResult restore_snapshot(int vm_fd, MemoryManager& memory,
                              std::vector<VcpuSnapshot>& vcpu_states,
                              const std::string& input_path);

    VmResult enable_dirty_tracking(int vm_fd, MemoryManager& memory);
    VmResult capture_dirty_pages(int vm_fd, MemoryManager& memory,
                                 std::vector<uint8_t>& dirty_bitmap,
                                 std::vector<uint64_t>& dirty_pages);

    [[nodiscard]] uint64_t last_snapshot_time() const noexcept { return last_snapshot_time_; }

private:
    VmResult write_snapshot_file(const std::string& path,
                                 const std::vector<uint8_t>& memory_data,
                                 const std::vector<uint8_t>& dirty_bitmap,
                                 const std::vector<VcpuSnapshot>& vcpu_states,
                                 bool incremental);
    uint32_t compute_checksum(const uint8_t* data, size_t size);
    uint64_t last_snapshot_time_{0};
};

} // namespace domvirt::hypervisor
