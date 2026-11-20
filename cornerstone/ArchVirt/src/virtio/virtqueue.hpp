#pragma once
#include <cstdint>
#include <atomic>
#include <vector>
#include <span>
#include <functional>
#include <memory>
#include "domvirt_types.hpp"

namespace domvirt::virtio {

struct VirtioDescriptor {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

static constexpr uint16_t VIRTQ_DESC_F_NEXT = 1;
static constexpr uint16_t VIRTQ_DESC_F_WRITE = 2;
static constexpr uint16_t VIRTQ_DESC_F_INDIRECT = 4;

struct SplitVirtqueue {
    uint16_t queue_size;
    uint16_t last_avail_idx{0};
    uint16_t last_used_idx{0};
    uint16_t free_head{0};
    uint64_t desc_addr;
    uint64_t avail_addr;
    uint64_t used_addr;
};

class Virtqueue {
public:
    explicit Virtqueue(uint16_t queue_size);
    ~Virtqueue() = default;

    Virtqueue(const Virtqueue&) = delete;
    Virtqueue& operator=(const Virtqueue&) = delete;

    bool init(uint64_t desc_addr, uint64_t avail_addr, uint64_t used_addr);
    void reset();

    bool avail_notify(uint8_t* guest_mem_base);
    bool process_descriptors(uint8_t* guest_mem_base,
                             std::function<void(std::span<uint8_t>, bool)> callback);

    void add_used(uint16_t id, uint32_t len);
    void signal_used(uint8_t* guest_mem_base);

    [[nodiscard]] uint16_t queue_size() const noexcept { return queue_size_; }
    [[nodiscard]] bool is_ready() const noexcept { return ready_; }

private:
    SplitVirtqueue vq_{};
    uint16_t queue_size_;
    bool ready_{false};
};

class VirtioTransport {
public:
    virtual ~VirtioTransport() = default;

    virtual uint64_t read_config(uint32_t offset, uint32_t size) = 0;
    virtual void write_config(uint32_t offset, uint64_t value, uint32_t size) = 0;
    virtual uint8_t read_status() = 0;
    virtual void write_status(uint8_t status) = 0;
    virtual uint32_t read_features() = 0;
    virtual void write_features(uint32_t features) = 0;
    virtual void notify_queue(uint16_t queue_idx) = 0;

    virtual bool mmio_read(uint64_t addr, uint8_t* data, uint32_t len) = 0;
    virtual bool mmio_write(uint64_t addr, const uint8_t* data, uint32_t len) = 0;
};

enum class VirtioDeviceStatus : uint8_t {
    ACKNOWLEDGE = 1,
    DRIVER = 2,
    DRIVER_OK = 4,
    FEATURES_OK = 8,
    DEVICE_NEEDS_RESET = 64,
    FAILED = 128,
};

} // namespace domvirt::virtio
