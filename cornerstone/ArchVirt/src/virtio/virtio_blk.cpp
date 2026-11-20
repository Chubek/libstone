#include "virtio_devices.hpp"
#include <cstring>

namespace domvirt::virtio {

VirtioBlk::VirtioBlk()
    : VirtioDevice(DEVICE_ID) {}

VmResult VirtioBlk::init() {
    config_.num_queues = 1;
    queues_.resize(1);
    queues_[0] = std::make_unique<Virtqueue>(128);
    initialized_ = true;
    return {VmErrorCode::SUCCESS, "VirtioBlk initialized"};
}

bool VirtioBlk::handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) {
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
    default:
        return VirtioDevice::handle_mmio_write(offset, data, len);
    }
}

void VirtioBlk::handle_queue_notify(uint16_t queue_idx) {
    if (queue_idx == 0 && guest_mem_ && !queues_.empty()) {
        queues_[0]->process_descriptors(guest_mem_,
            [](std::span<uint8_t> buf, bool write) {
                (void)buf; (void)write;
            });
    }
}

} // namespace domvirt::virtio
