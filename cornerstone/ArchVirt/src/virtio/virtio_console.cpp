#include "virtio_devices.hpp"
#include <cstring>

namespace domvirt::virtio {

VirtioConsole::VirtioConsole()
    : VirtioDevice(DEVICE_ID) {}

VmResult VirtioConsole::init() {
    config_.num_queues = 2;
    queues_.resize(2);
    queues_[0] = std::make_unique<Virtqueue>(64);
    queues_[1] = std::make_unique<Virtqueue>(64);
    initialized_ = true;
    return {VmErrorCode::SUCCESS, "VirtioConsole initialized"};
}

bool VirtioConsole::handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) {
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

void VirtioConsole::handle_queue_notify(uint16_t queue_idx) {
    if (queue_idx == 0 && guest_mem_ && !queues_.empty()) {
        queues_[0]->process_descriptors(guest_mem_,
            [](std::span<uint8_t> buf, bool write) {
                if (!write && !buf.empty()) {
                    buf[0] = 0;
                }
            });
    }
}

} // namespace domvirt::virtio
