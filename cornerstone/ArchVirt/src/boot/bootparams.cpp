#include "loader.hpp"
#include <cstring>
#include <algorithm>

namespace domvirt::boot {

namespace {

void write_e820_table(uint8_t* mem, uint64_t addr,
                      const std::vector<E820Entry>& entries) {
    uint32_t count = std::min(entries.size(), size_t(128));
    memcpy(mem + addr, &count, 4);

    for (size_t i = 0; i < count; ++i) {
        uint64_t entry_addr = addr + 4 + i * 20;
        memcpy(mem + entry_addr, &entries[i].addr, 8);
        memcpy(mem + entry_addr + 8, &entries[i].size, 8);
        memcpy(mem + entry_addr + 16, &entries[i].type, 4);
    }
}

} // namespace

VmResult BootLoader::write_boot_params(uint8_t* mem, uint64_t addr) {
    if (e820_map_.empty()) {
        add_e820_entry(0, 0x9FC00, 1);
        add_e820_entry(0x9FC00, 0x400, 2);
        add_e820_entry(0xF0000, 0x10000, 2);
        add_e820_entry(0x100000, 0x7F00000, 1);
    }

    if (!cmdline_.empty()) {
        boot_params_.cmd_line_ptr = CMDLINE_BASE;
        memcpy(mem + CMDLINE_BASE, cmdline_.c_str(),
               std::min(cmdline_.size() + 1, size_t(2048)));
    }

    memcpy(mem + addr, &boot_params_, sizeof(BootParams));

    uint64_t e820_addr = addr + 0x2D0;
    write_e820_table(mem, e820_addr, e820_map_);

    return {VmErrorCode::SUCCESS, "Boot params written"};
}

VmResult BootLoader::write_graphics_info(uint8_t* mem, const GuestGraphicsInfo& info,
                                         uint64_t addr) {
    if (mem == nullptr) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid guest memory"};
    }
    std::memcpy(mem + addr, &info, sizeof(info));
    return {VmErrorCode::SUCCESS, "Graphics info written"};
}

} // namespace domvirt::boot
