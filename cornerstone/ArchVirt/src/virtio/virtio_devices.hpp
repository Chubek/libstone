#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include "virtqueue.hpp"
#include "domvirt_types.hpp"

namespace domvirt::virtio {

struct VirtioDeviceConfig {
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t device_features;
    uint32_t device_features_sel;
    uint32_t driver_features;
    uint32_t driver_features_sel;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t num_queues;
    uint16_t queue_sel;
    uint16_t queue_size;
    uint16_t queue_notify;
};

class VirtioDevice {
public:
    explicit VirtioDevice(uint32_t device_id);
    virtual ~VirtioDevice() = default;

    virtual VmResult init() = 0;
    virtual void reset();

    virtual bool handle_mmio_read(uint64_t offset, uint8_t* data, uint32_t len);
    virtual bool handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len);
    virtual void handle_queue_notify(uint16_t queue_idx);

    [[nodiscard]] uint32_t device_id() const noexcept { return config_.device_id; }
    [[nodiscard]] VirtioDeviceConfig& config() noexcept { return config_; }
    [[nodiscard]] uint8_t status() const noexcept { return config_.device_status; }

    void set_guest_memory(uint8_t* base) { guest_mem_ = base; }
    void set_irq_callback(std::function<void()> cb) { irq_cb_ = std::move(cb); }

protected:
    VirtioDeviceConfig config_{};
    std::vector<std::unique_ptr<Virtqueue>> queues_;
    uint8_t* guest_mem_{nullptr};
    std::function<void()> irq_cb_;
    bool initialized_{false};
};

class VirtioNet : public VirtioDevice {
public:
    VirtioNet();
    VmResult init() override;
    bool handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) override;
    void handle_queue_notify(uint16_t queue_idx) override;

    static constexpr uint32_t DEVICE_ID = 1;
    uint16_t rx_queue{0}, tx_queue{1};
};

class VirtioBlk : public VirtioDevice {
public:
    VirtioBlk();
    VmResult init() override;
    bool handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) override;
    void handle_queue_notify(uint16_t queue_idx) override;

    static constexpr uint32_t DEVICE_ID = 2;
    uint64_t capacity{0};
};

class VirtioConsole : public VirtioDevice {
public:
    VirtioConsole();
    VmResult init() override;
    bool handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) override;
    void handle_queue_notify(uint16_t queue_idx) override;

    static constexpr uint32_t DEVICE_ID = 3;
    uint16_t rx_queue{0}, tx_queue{1};
};

} // namespace domvirt::virtio
