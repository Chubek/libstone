#include "pc_devices.hpp"
#include <utility>

namespace domvirt::machine {
PcDevices::PcDevices(std::vector<std::uint8_t> disk, Serial serial)
    : disk_(std::move(disk)), serial_(std::move(serial)) { ata_[7] = 0x40; }

bool PcDevices::io(std::uint16_t port, bool write, unsigned size, std::uint32_t& v) {
    if (port == 0x1f0 && size == 2) {
        if (remaining_ < 2 || writing_ != write || !(ata_[7] & 8)) {
            ata_[1] = 4; ata_[7] = 0x41; remaining_ = 0; v = 0xffff; return true;
        }
        if (write) { disk_[at_] = v; disk_[at_+1] = v >> 8; }
        else v = disk_[at_] | (std::uint32_t(disk_[at_+1]) << 8);
        at_ += 2; remaining_ -= 2;
        if (!remaining_) ata_[7] = 0x40;
        return true;
    }
    if (size != 1) return false;
    if (port >= 0x1f1 && port <= 0x1f7) {
        unsigned n = port - 0x1f0;
        if (!write) { v = ata_[n]; return true; }
        if (n != 7) { ata_[n] = v; return true; }
        remaining_ = 0; ata_[1] = 4; ata_[7] = 0x41;
        const std::uint32_t lba = ata_[3] | (std::uint32_t(ata_[4]) << 8) |
            (std::uint32_t(ata_[5]) << 16) | (std::uint32_t(ata_[6] & 15) << 24);
        if ((v != 0x20 && v != 0x30) || ata_[2] != 1 ||
            (ata_[6] & 0xf0) != 0xe0 || lba >= disk_.size()/512 || (control_ & 4)) return true;
        at_ = std::size_t(lba)*512; remaining_ = 512; writing_ = v == 0x30;
        ata_[1] = 0; ata_[7] = 0x48; return true;
    }
    if (port == 0x3f6) {
        if (write) { control_ = v; remaining_ = 0; ata_[7] = (v & 4) ? 0x80 : 0x40; }
        else v = ata_[7];
        return true;
    }
    if (port >= 0x3f8 && port <= 0x3ff) {
        unsigned n = port - 0x3f8;
        if ((uart_[3] & 0x80) && n < 2) { if (write) uart_[n] = v; else v = uart_[n]; return true; }
        if (n == 0) {
            if (write) { if (serial_) serial_(true, v); }
            else { v = input_ ? std::exchange(input_, 0) : serial_ ? serial_(false, 0) : 0; }
        } else if (!write && n == 5) {
            if (!input_ && serial_) input_ = serial_(false, 0);
            v = 0x60 | (input_ != 0);
        } else if (write) uart_[n] = v;
        else v = n == 2 ? 1 : uart_[n];
        return true;
    }
    // A20 is permanently enabled; port 92 reports that platform contract.
    if (port == 0x92) { if (write) a20_ = (v & 0xfc) | 2; else v = a20_; return true; }
    // Firmware masks both PICs; this polled platform injects no interrupts.
    if (port == 0x21 || port == 0xa1 || port == 0x80) { if (!write) v = 0xff; return true; }
    return false;
}
}
