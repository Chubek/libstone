#include "virtio_devices.hpp"
#include <cstring>
#include <algorithm>

namespace domvirt::virtio {

VirtioNet::VirtioNet()
    : VirtioDevice(DEVICE_ID) {}

VmResult VirtioNet::init() {
    config_.num_queues = 2;
    queues_.resize(2);
    queues_[0] = std::make_unique<Virtqueue>(256);
    queues_[1] = std::make_unique<Virtqueue>(256);
    initialized_ = true;
    return {VmErrorCode::SUCCESS, "VirtioNet initialized"};
}

bool VirtioNet::handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) {
    switch (offset) {
    case 0x08: {
        uint32_t val = 0;
        memcpy(&val, data, std::min(len, 4u));
        config_.driver_features = val;
        return true;
    }
    case 0x14: {
        uint16_t val = 0;
        memcpy(&val, data, std::min(len, 2u));
        config_.queue_sel = val;
        return true;
    }
    case 0x18: {
        if (config_.queue_sel == 0 && !queues_.empty()) {
            uint64_t val = 0;
            memcpy(&val, data, std::min(len, 8u));
            queues_[0]->init(val, 0, 0);
        }
        return true;
    }
    case 0x28: {
        if (config_.queue_sel == 1 && queues_.size() > 1) {
            uint64_t val = 0;
            memcpy(&val, data, std::min(len, 8u));
            queues_[1]->init(val, 0, 0);
        }
        return true;
    }
    default:
        return VirtioDevice::handle_mmio_write(offset, data, len);
    }
}

void VirtioNet::handle_queue_notify(uint16_t queue_idx) {
    if (queue_idx == 1 && guest_mem_ && queues_.size() > 1) {
        queues_[1]->process_descriptors(guest_mem_,
            [](std::span<uint8_t> buf, bool write) {
                (void)buf; (void)write;
            });
    }
}

} // namespace domvirt::virtio
