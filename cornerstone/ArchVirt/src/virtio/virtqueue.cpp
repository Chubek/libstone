#include "virtqueue.hpp"
#include <cstring>
#include <algorithm>
#include <atomic>

namespace domvirt::virtio {

struct AvailRing {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[1];
};

struct UsedRing {
    uint16_t flags;
    uint16_t idx;
    struct { uint32_t id; uint32_t len; } ring[1];
};

static constexpr std::size_t kAvailFlagsOffset = 0;
static constexpr std::size_t kAvailIdxOffset = 2;
static constexpr std::size_t kAvailRingOffset = 4;
static constexpr std::size_t kUsedFlagsOffset = 0;
static constexpr std::size_t kUsedIdxOffset = 2;

Virtqueue::Virtqueue(uint16_t queue_size)
    : queue_size_(queue_size) {}

bool Virtqueue::init(uint64_t desc_addr, uint64_t avail_addr, uint64_t used_addr) {
    if (queue_size_ == 0 || queue_size_ > 1024 ||
        (queue_size_ & (queue_size_ - 1)) != 0) {
        return false;
    }
    vq_.queue_size = queue_size_;
    vq_.desc_addr = desc_addr;
    vq_.avail_addr = avail_addr;
    vq_.used_addr = used_addr;
    ready_ = true;
    return true;
}

void Virtqueue::reset() {
    vq_.last_avail_idx = 0;
    vq_.last_used_idx = 0;
    vq_.free_head = 0;
    ready_ = false;
}

bool Virtqueue::avail_notify(uint8_t* guest_mem_base) {
    if (!ready_ || !guest_mem_base) return false;

    uint16_t avail_idx = 0;
    std::memcpy(&avail_idx,
                guest_mem_base + vq_.avail_addr + kAvailIdxOffset,
                sizeof(avail_idx));
    std::atomic_thread_fence(std::memory_order_acquire);
    return avail_idx != vq_.last_avail_idx;
}

bool Virtqueue::process_descriptors(uint8_t* guest_mem_base,
    std::function<void(std::span<uint8_t>, bool)> callback) {
    if (!ready_ || !guest_mem_base || !callback) return false;

    uint16_t avail_idx = 0;
    std::memcpy(&avail_idx,
                guest_mem_base + vq_.avail_addr + kAvailIdxOffset,
                sizeof(avail_idx));
    std::atomic_thread_fence(std::memory_order_acquire);

    while (vq_.last_avail_idx != avail_idx) {
        uint8_t* avail_ring = guest_mem_base + vq_.avail_addr + kAvailRingOffset;
        uint16_t head = 0;
        std::memcpy(&head,
                    avail_ring + (vq_.last_avail_idx % vq_.queue_size) * sizeof(uint16_t),
                    sizeof(head));

        if (head >= queue_size_) return false;
        uint16_t desc_idx = head;
        uint16_t traversed = 0;
        do {
            if (desc_idx >= queue_size_ || traversed++ >= queue_size_) return false;
            VirtioDescriptor desc{};
            std::memcpy(&desc,
                        guest_mem_base + vq_.desc_addr + desc_idx * sizeof(VirtioDescriptor),
                        sizeof(desc));
            bool is_write = (desc.flags & VIRTQ_DESC_F_WRITE) != 0;
            auto span = std::span<uint8_t>(
                guest_mem_base + desc.addr, desc.len);
            callback(span, is_write);
            if ((desc.flags & VIRTQ_DESC_F_NEXT) == 0) break;
            desc_idx = desc.next;
        } while (true);

        add_used(head, 0);
        vq_.last_avail_idx++;
    }

    signal_used(guest_mem_base);
    return true;
}

void Virtqueue::add_used(uint16_t id, uint32_t len) {
    vq_.last_used_idx++;
}

void Virtqueue::signal_used(uint8_t* guest_mem_base) {
    if (!ready_ || !guest_mem_base) return;
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(guest_mem_base + vq_.used_addr + kUsedIdxOffset,
                &vq_.last_used_idx,
                sizeof(vq_.last_used_idx));
}

} // namespace domvirt::virtio
