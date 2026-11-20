#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include "domvirt_types.hpp"

namespace domvirt::boot {

struct E820Entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
};

struct BootParams {
    uint8_t setup_sects;
    uint16_t root_flags;
    uint32_t syssize;
    uint16_t ram_size;
    uint16_t vid_mode;
    uint16_t root_dev;
    uint16_t boot_flag;
    uint32_t header_magic;
    uint32_t header_version;
    uint32_t realmode_swtch;
    uint16_t start_sys_seg;
    uint16_t kernel_version_ptr;
    uint8_t type_of_loader;
    uint8_t loadflags;
    uint16_t setup_move_size;
    uint32_t code32_start;
    uint32_t ramdisk_image;
    uint32_t ramdisk_size;
    uint32_t boot_sects_kludge;
    uint16_t heap_end_ptr;
    uint16_t ext_loader_ver;
    uint8_t ext_loader_type;
    uint32_t cmd_line_ptr;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t relocatable_kernel;
    uint8_t min_alignment;
    uint16_t xloadflags;
    uint32_t cmdline_size;
    uint32_t hardware_subarch;
    uint64_t hardware_subarch_data;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint64_t setup_data;
    uint64_t pref_address;
    uint32_t init_size;
    uint32_t handover_offset;
};

struct GuestGraphicsInfo {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t width;
    uint16_t height;
    uint32_t pitch;
    uint32_t format;
    uint64_t framebuffer_gpa;
    uint64_t framebuffer_size;
};

class BootLoader {
public:
    BootLoader();
    ~BootLoader() = default;

    VmResult load_bzimage(const std::string& path, uint8_t* guest_mem,
                          uint64_t mem_size, uint64_t& entry_point);
    VmResult load_elf(const std::string& path, uint8_t* guest_mem,
                      uint64_t mem_size, uint64_t& entry_point, uint16_t machine = 62);
    VmResult load_elf(std::span<const uint8_t> data, uint8_t* guest_mem,
                      uint64_t mem_size, uint64_t& entry_point, uint16_t machine = 62,
                      uint64_t load_begin = 0, uint64_t load_end = UINT64_MAX);
    VmResult load_blob(const std::string& path, uint8_t* guest_mem,
                       uint64_t mem_size, uint64_t guest_addr,
                       uint64_t& blob_size);
    VmResult load_blob(const uint8_t* data, uint64_t size, uint8_t* guest_mem,
                       uint64_t mem_size, uint64_t guest_addr,
                       uint64_t& blob_size);
    VmResult load_initrd(const std::string& path, uint8_t* guest_mem,
                         uint64_t mem_size, uint64_t& initrd_addr, uint64_t& initrd_size);
    VmResult load_initrd(const uint8_t* data, uint64_t size, uint8_t* guest_mem,
                         uint64_t mem_size, uint64_t& initrd_addr,
                         uint64_t& initrd_size);
    VmResult write_boot_params(uint8_t* guest_mem, uint64_t addr = BOOT_PARAMS_BASE);
    VmResult write_graphics_info(uint8_t* guest_mem, const GuestGraphicsInfo& info,
                                 uint64_t addr = GRAPHICS_INFO_BASE);

    void set_cmdline(const std::string& cmdline) { cmdline_ = cmdline; }
    void add_e820_entry(uint64_t addr, uint64_t size, uint32_t type);

    [[nodiscard]] const std::vector<E820Entry>& e820_map() const noexcept { return e820_map_; }
    [[nodiscard]] const BootParams& boot_params() const noexcept { return boot_params_; }

    static constexpr uint64_t BOOT_PARAMS_BASE = 0x10000;
    static constexpr uint64_t GRAPHICS_INFO_BASE = 0x11000;
    static constexpr uint64_t CMDLINE_BASE = 0x20000;
    static constexpr uint64_t SETUP_BASE = 0x100000;
    static constexpr uint64_t KERNEL_LOAD_BASE = 0x1800000;

private:
    VmResult parse_bzimage_header(const std::vector<uint8_t>& data, BootParams& params);
    BootParams boot_params_{};
    std::vector<E820Entry> e820_map_;
    std::string cmdline_;
};

} // namespace domvirt::boot
