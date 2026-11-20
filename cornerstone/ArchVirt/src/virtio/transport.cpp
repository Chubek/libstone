#include "virtio_devices.hpp"
#include <cstring>

namespace domvirt::virtio {

class MmioTransport : public VirtioTransport {
public:
    explicit MmioTransport(VirtioDevice& device, uint64_t base_addr)
        : device_(device), base_addr_(base_addr) {}

    uint64_t read_config(uint32_t offset, uint32_t size) override {
        uint8_t buf[8] = {};
        device_.handle_mmio_read(offset, buf, size);
        uint64_t val = 0;
        memcpy(&val, buf, std::min(size, 8u));
        return val;
    }

    void write_config(uint32_t offset, uint64_t value, uint32_t size) override {
        if (size == 0 || size > sizeof(value)) return;
        uint8_t buf[8];
        memcpy(buf, &value, size);
        device_.handle_mmio_write(offset, buf, size);
    }

    uint8_t read_status() override {
        return device_.config().device_status;
    }

    void write_status(uint8_t status) override {
        device_.config().device_status = status;
    }

    uint32_t read_features() override {
        return device_.config().device_features;
    }

    void write_features(uint32_t features) override {
        device_.config().driver_features = features;
    }

    void notify_queue(uint16_t queue_idx) override {
        device_.handle_queue_notify(queue_idx);
    }

    bool mmio_read(uint64_t addr, uint8_t* data, uint32_t len) override {
        if (!data || len == 0 || addr < base_addr_) return false;
        uint64_t offset = addr - base_addr_;
        if (offset < 0x100) {
            return device_.handle_mmio_read(offset, data, len);
        }
        return false;
    }

    bool mmio_write(uint64_t addr, const uint8_t* data, uint32_t len) override {
        if (!data || len == 0 || addr < base_addr_) return false;
        uint64_t offset = addr - base_addr_;
        if (offset < 0x100) {
            return device_.handle_mmio_write(offset, data, len);
        }
        return false;
    }

private:
    VirtioDevice& device_;
    uint64_t base_addr_;
};

} // namespace domvirt::virtio
