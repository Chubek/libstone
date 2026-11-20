#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace domvirt::machine {
// Single primary ATA master, LBA28, one PIO sector per command. Writes modify
// this VM's private image only. No IRQ, DMA, bus mastering or persistence.
class PcDevices {
public:
    using Serial = std::function<std::uint8_t(bool, std::uint8_t)>;
    PcDevices(std::vector<std::uint8_t> disk, Serial serial);
    bool io(std::uint16_t port, bool write, unsigned size, std::uint32_t& value);
private:
    std::vector<std::uint8_t> disk_;
    Serial serial_;
    std::array<std::uint8_t, 8> ata_{};
    std::array<std::uint8_t, 8> uart_{};
    std::size_t at_{0}, remaining_{0};
    bool writing_{false};
    std::uint8_t input_{0}, control_{0}, a20_{2};
};
}
