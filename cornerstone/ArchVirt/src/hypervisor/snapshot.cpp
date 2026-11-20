#include "snapshot.hpp"
#include <fstream>
#include <sys/ioctl.h>
#include <cstring>
#include <cerrno>
#include <chrono>

namespace domvirt::hypervisor {

VmResult SnapshotEngine::create_full_snapshot(
    int vm_fd, MemoryManager& memory,
    const std::vector<VcpuSnapshot>& vcpu_states,
    const std::string& output_path) {

    std::vector<uint8_t> memory_data;
    for (auto& slot : memory.slots()) {
        size_t sz = slot.memory_size;
        auto* ptr = static_cast<uint8_t*>(slot.mmap_base);
        if (ptr && sz > 0) {
            memory_data.insert(memory_data.end(), ptr, ptr + sz);
        }
    }

    std::vector<uint8_t> dirty_bitmap;
    return write_snapshot_file(output_path, memory_data, dirty_bitmap,
                               vcpu_states, false);
}

VmResult SnapshotEngine::create_incremental_snapshot(
    int vm_fd, MemoryManager& memory,
    const std::vector<VcpuSnapshot>& vcpu_states,
    const std::string& output_path) {

    std::vector<uint8_t> dirty_bitmap;
    std::vector<uint64_t> dirty_pages;
    auto result = capture_dirty_pages(vm_fd, memory, dirty_bitmap, dirty_pages);
    if (!result) return result;

    std::vector<uint8_t> memory_data;
    for (auto gpa : dirty_pages) {
        auto* ptr = memory.translate(gpa);
        if (ptr) {
            memory_data.insert(memory_data.end(), ptr, ptr + PAGE_SIZE);
        }
    }

    return write_snapshot_file(output_path, memory_data, dirty_bitmap,
                               vcpu_states, true);
}

VmResult SnapshotEngine::restore_snapshot(
    int vm_fd, MemoryManager& memory,
    std::vector<VcpuSnapshot>& vcpu_states,
    const std::string& input_path) {

    std::ifstream file(input_path, std::ios::binary);
    if (!file.is_open()) {
        return {VmErrorCode::SNAPSHOT_ERROR,
                "Cannot open snapshot: " + input_path};
    }

    SnapshotHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (file.gcount() != sizeof(header)) {
        return {VmErrorCode::SNAPSHOT_ERROR, "Failed to read snapshot header"};
    }

    if (std::string(header.magic, 8) != "DOMSNAP") {
        return {VmErrorCode::SNAPSHOT_ERROR, "Invalid snapshot magic"};
    }

    file.seekg(header.memory_offset);
    for (auto& slot : memory.slots()) {
        size_t sz = slot.memory_size;
        auto* ptr = static_cast<uint8_t*>(slot.mmap_base);
        if (ptr && sz > 0) {
            file.read(reinterpret_cast<char*>(ptr), sz);
        }
    }

    file.seekg(header.vcpu_state_offset);
    for (auto& vcpu : vcpu_states) {
        file.read(reinterpret_cast<char*>(&vcpu), sizeof(VcpuSnapshot));
    }

    last_snapshot_time_ = header.timestamp;
    return {VmErrorCode::SUCCESS, "Snapshot restored"};
}

VmResult SnapshotEngine::enable_dirty_tracking(int vm_fd, MemoryManager& memory) {
    for (auto& slot : memory.slots()) {
        auto result = memory.enable_dirty_log(vm_fd, slot.slot_id);
        if (!result) return result;
    }
    return {VmErrorCode::SUCCESS, "Dirty tracking enabled"};
}

VmResult SnapshotEngine::capture_dirty_pages(
    int vm_fd, MemoryManager& memory,
    std::vector<uint8_t>& dirty_bitmap,
    std::vector<uint64_t>& dirty_pages) {

    dirty_pages.clear();
    for (auto& slot : memory.slots()) {
        size_t bitmap_size = (slot.memory_size / PAGE_SIZE + 7) / 8;
        dirty_bitmap.assign(bitmap_size, 0);

        struct kvm_dirty_log log{};
        log.slot = slot.slot_id;
        log.dirty_bitmap = dirty_bitmap.data();

        if (ioctl(vm_fd, KVM_GET_DIRTY_LOG, &log) < 0) {
            return {VmErrorCode::SNAPSHOT_ERROR,
                    "KVM_GET_DIRTY_LOG failed: " + std::string(strerror(errno))};
        }

        for (size_t i = 0; i < bitmap_size; ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                if (dirty_bitmap[i] & (1 << bit)) {
                    uint64_t gpa = slot.guest_phys_addr +
                                   (i * 8 + bit) * static_cast<size_t>(PAGE_SIZE);
                    dirty_pages.push_back(gpa);
                }
            }
        }
    }

    return {VmErrorCode::SUCCESS, "Captured " + std::to_string(dirty_pages.size()) + " dirty pages"};
}

VmResult SnapshotEngine::write_snapshot_file(
    const std::string& path,
    const std::vector<uint8_t>& memory_data,
    const std::vector<uint8_t>& dirty_bitmap,
    const std::vector<VcpuSnapshot>& vcpu_states,
    bool incremental) {

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {VmErrorCode::SNAPSHOT_ERROR,
                "Cannot create snapshot: " + path};
    }

    SnapshotHeader header{};
    auto now = std::chrono::system_clock::now();
    header.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    header.memory_size = memory_data.size();
    header.num_vcpus = static_cast<uint32_t>(vcpu_states.size());
    header.num_dirty_pages = static_cast<uint32_t>(dirty_bitmap.size() * 8);

    uint64_t offset = sizeof(SnapshotHeader);
    header.dirty_bitmap_offset = offset;
    offset += dirty_bitmap.size();

    header.memory_offset = offset;
    offset += memory_data.size();

    header.vcpu_state_offset = offset;

    header.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(memory_data.data()),
                                       memory_data.size());

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(dirty_bitmap.data()), dirty_bitmap.size());
    file.write(reinterpret_cast<const char*>(memory_data.data()), memory_data.size());

    for (auto& vcpu : vcpu_states) {
        file.write(reinterpret_cast<const char*>(&vcpu), sizeof(VcpuSnapshot));
    }

    last_snapshot_time_ = header.timestamp;
    return {VmErrorCode::SUCCESS, "Snapshot written to " + path};
}

uint32_t SnapshotEngine::compute_checksum(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; ++i) {
        sum = (sum << 1) ^ data[i];
    }
    return sum;
}

} // namespace domvirt::hypervisor
