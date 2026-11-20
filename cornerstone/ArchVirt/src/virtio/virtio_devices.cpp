#include "virtio_devices.hpp"
#include <cstring>

namespace domvirt::virtio {

VirtioDevice::VirtioDevice(uint32_t device_id) {
    config_.device_id = device_id;
    config_.vendor_id = 0x1AF4;
    config_.device_status = 0;
}

void VirtioDevice::reset() {
    config_.device_status = 0;
    config_.driver_features = 0;
    config_.device_features_sel = 0;
    config_.driver_features_sel = 0;
    initialized_ = false;
}

bool VirtioDevice::handle_mmio_read(uint64_t offset, uint8_t* data, uint32_t len) {
    memset(data, 0, len);
    switch (offset) {
    case 0x00: memcpy(data, &config_.device_id, std::min(len, 4u)); return true;
    case 0x04: memcpy(data, &config_.vendor_id, std::min(len, 4u)); return true;
    case 0x08: memcpy(data, &config_.device_features, std::min(len, 4u)); return true;
    case 0x0C: memcpy(data, &config_.device_features_sel, std::min(len, 4u)); return true;
    case 0x10: memcpy(data, &config_.driver_features, std::min(len, 4u)); return true;
    case 0x14: memcpy(data, &config_.queue_sel, std::min(len, 2u)); return true;
    case 0x16: memcpy(data, &config_.queue_size, std::min(len, 2u)); return true;
    case 0x18: memcpy(data, &config_.queue_notify, std::min(len, 2u)); return true;
    case 0x1C: memcpy(data, &config_.device_status, std::min(len, 1u)); return true;
    default: return false;
    }
}

bool VirtioDevice::handle_mmio_write(uint64_t offset, const uint8_t* data, uint32_t len) {
    switch (offset) {
    case 0x0C: memcpy(&config_.device_features_sel, data, std::min(len, 4u)); return true;
    case 0x10: memcpy(&config_.driver_features, data, std::min(len, 4u)); return true;
    case 0x14: memcpy(&config_.queue_sel, data, std::min(len, 2u)); return true;
    case 0x16: memcpy(&config_.queue_size, data, std::min(len, 2u)); return true;
    case 0x1C: memcpy(&config_.device_status, data, std::min(len, 1u)); return true;
    default: return false;
    }
}

void VirtioDevice::handle_queue_notify(uint16_t queue_idx) {
    (void)queue_idx;
}

} // namespace domvirt::virtio
