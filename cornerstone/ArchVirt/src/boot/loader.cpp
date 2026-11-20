#include "loader.hpp"
#include <fstream>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>

namespace domvirt::boot {

BootLoader::BootLoader() {
    memset(&boot_params_, 0, sizeof(boot_params_));
    boot_params_.boot_flag = 0xAA55;
    boot_params_.header_magic = 0x53726448;
    boot_params_.header_version = 0x020E;
    boot_params_.kernel_alignment = 0x200000;
    boot_params_.relocatable_kernel = 1;
    boot_params_.xloadflags = 0x03;
    boot_params_.cmdline_size = 2048;
    boot_params_.initrd_addr_max = 0x37FFFFFF;
}

VmResult BootLoader::load_bzimage(const std::string& path, uint8_t* guest_mem,
                                  uint64_t mem_size, uint64_t& entry_point) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {VmErrorCode::BOOT_LOAD_FAILED,
                "Cannot open: " + path};
    }

    auto size = file.tellg();
    file.seekg(0);

    if (size <= 0 || guest_mem == nullptr || mem_size == 0) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid guest memory or empty image"};
    }
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (size < 0x268) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Image too small"};
    }

    auto result = parse_bzimage_header(data, boot_params_);
    if (!result) return result;

    uint32_t setup_sects = boot_params_.setup_sects;
    if (setup_sects == 0) setup_sects = 4;
    uint32_t setup_size = (setup_sects + 1) * 512;

    if (static_cast<uint64_t>(size) < setup_size) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Setup section truncated"};
    }

    const size_t setup_copy_size = std::min(
        static_cast<size_t>(size), static_cast<size_t>(setup_size));
    if (SETUP_BASE + setup_copy_size > mem_size) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Setup section does not fit guest memory"};
    }
    memcpy(guest_mem + SETUP_BASE, data.data(), setup_copy_size);

    uint64_t kernel_offset = setup_size;
    uint64_t kernel_size = static_cast<uint64_t>(size) - kernel_offset;

    if (kernel_size > 0) {
        if (KERNEL_LOAD_BASE + kernel_size > mem_size) {
            return {VmErrorCode::BOOT_LOAD_FAILED, "Kernel does not fit guest memory"};
        }
        memcpy(guest_mem + KERNEL_LOAD_BASE, data.data() + kernel_offset, kernel_size);
    }

    if (!cmdline_.empty()) {
        memcpy(guest_mem + CMDLINE_BASE, cmdline_.c_str(),
               std::min(cmdline_.size() + 1, size_t(2048)));
        boot_params_.cmd_line_ptr = CMDLINE_BASE;
    }

    memcpy(guest_mem + BOOT_PARAMS_BASE, &boot_params_, sizeof(BootParams));

    entry_point = KERNEL_LOAD_BASE;
    return {VmErrorCode::SUCCESS, "bzImage loaded"};
}

VmResult BootLoader::load_elf(const std::string& path, uint8_t* guest_mem,
                              uint64_t mem_size, uint64_t& entry_point, uint16_t machine) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Cannot open: " + path};
    }

    auto size = file.tellg();
    file.seekg(0);

    if (size <= 0 || guest_mem == nullptr || mem_size == 0) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid guest memory or empty ELF"};
    }
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file) return {VmErrorCode::BOOT_LOAD_FAILED, "Truncated ELF read"};
    return load_elf(data, guest_mem, mem_size, entry_point, machine);
}

VmResult BootLoader::load_elf(std::span<const uint8_t> data, uint8_t* guest_mem,
                              uint64_t mem_size, uint64_t& entry_point, uint16_t machine,
                              uint64_t load_begin, uint64_t load_end) {
    auto fail = [](const char* message) { return VmResult{VmErrorCode::BOOT_LOAD_FAILED, message}; };
    if (!guest_mem || data.size() < 64) return fail("ELF too small or guest memory missing");
    auto le = [&](size_t offset, unsigned count) {
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= uint64_t(data[offset + i]) << (8 * i);
        return value;
    };
    if (memcmp(data.data(), "\177ELF", 4) || data[4] != 2 || data[5] != 1 || data[6] != 1 ||
        le(16, 2) != 2 || le(20, 4) != 1 || le(52, 2) != 64)
        return fail("Expected a little-endian ELF64 executable");
    if (le(18, 2) != machine) return fail("ELF architecture does not match selected machine");
    if (machine == 243 && le(48, 4) != 0)
        return fail("RISC-V requires uncompressed RV64 soft-float LP64 ELF flags");
    const auto entry = le(24, 8), phoff = le(32, 8), phsize = le(54, 2), phnum = le(56, 2);
    if (phsize != 56 || !phnum || phoff > data.size() || phnum > (data.size() - phoff) / phsize)
        return fail("Invalid ELF program header table");
    load_end = std::min(load_end, mem_size);
    struct Segment { uint64_t offset, address, filesz, memsz; };
    std::vector<Segment> segments;
    bool executable_entry = false;
    for (uint64_t i = 0; i < phnum; ++i) {
        const auto p = phoff + i * phsize;
        if (le(p, 4) != 1) continue;
        Segment s{le(p + 8, 8), le(p + 24, 8), le(p + 32, 8), le(p + 40, 8)};
        const auto vaddr = le(p + 16, 8), align = le(p + 48, 8);
        if (s.address != vaddr || s.filesz > s.memsz || s.offset > data.size() ||
            s.filesz > data.size() - s.offset || s.address < load_begin || s.address > load_end ||
            s.memsz > load_end - s.address || (align > 1 && ((align & (align - 1)) ||
            (s.address & (align - 1)) != (s.offset & (align - 1)))))
            return fail("ELF segment is truncated, outside the boot layout, or not identity mapped");
        for (const auto& prior : segments)
            if (s.memsz && prior.memsz && s.address < prior.address + prior.memsz &&
                prior.address < s.address + s.memsz) return fail("ELF load segments overlap");
        if ((le(p + 4, 4) & 1) && entry >= s.address && entry - s.address < s.filesz &&
            (machine != 243 || s.filesz - (entry - s.address) >= 4))
            executable_entry = true;
        segments.push_back(s);
    }
    if (!executable_entry || ((machine == 183 || machine == 243) && (entry & 3)))
        return fail("ELF entry is not executable or aligned");
    // Validate every segment before touching guest RAM, including BSS-only ones.
    for (const auto& s : segments) {
        memcpy(guest_mem + s.address, data.data() + s.offset, s.filesz);
        memset(guest_mem + s.address + s.filesz, 0, s.memsz - s.filesz);
    }
    entry_point = entry;
    return {};
}

VmResult BootLoader::load_blob(const std::string& path, uint8_t* guest_mem,
                               uint64_t mem_size, uint64_t guest_addr,
                               uint64_t& blob_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Cannot open blob: " + path};
    }

    const auto size = static_cast<uint64_t>(file.tellg());
    file.seekg(0);

    if (guest_mem == nullptr || mem_size == 0) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid guest memory"};
    }
    if (guest_addr > mem_size || size > mem_size - guest_addr) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Blob does not fit guest memory"};
    }

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Failed to read blob: " + path};
    }
    return load_blob(data.data(), size, guest_mem, mem_size, guest_addr, blob_size);
}

VmResult BootLoader::load_blob(const uint8_t* data, uint64_t size,
                               uint8_t* guest_mem, uint64_t mem_size,
                               uint64_t guest_addr, uint64_t& blob_size) {
    if (data == nullptr || size == 0) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Empty blob"};
    }
    if (guest_mem == nullptr || mem_size == 0) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid guest memory"};
    }
    if (guest_addr > mem_size || size > mem_size - guest_addr) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Blob does not fit guest memory"};
    }
    memcpy(guest_mem + guest_addr, data, static_cast<std::size_t>(size));
    blob_size = size;
    return {VmErrorCode::SUCCESS, "Blob loaded"};
}

VmResult BootLoader::load_initrd(const std::string& path, uint8_t* guest_mem,
                                 uint64_t mem_size, uint64_t& initrd_addr,
                                 uint64_t& initrd_size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Cannot open initrd: " + path};
    }

    auto size = static_cast<uint64_t>(file.tellg());
    file.seekg(0);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Failed to read initrd: " + path};
    }
    return load_initrd(data.data(), size, guest_mem, mem_size, initrd_addr, initrd_size);
}

VmResult BootLoader::load_initrd(const uint8_t* data, uint64_t size,
                                 uint8_t* guest_mem, uint64_t mem_size,
                                 uint64_t& initrd_addr, uint64_t& initrd_size) {
    if (data == nullptr || size == 0) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Empty initrd"};
    }
    initrd_addr = 0x800000;
    initrd_size = size;
    if (initrd_addr > mem_size || initrd_size > mem_size - initrd_addr) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Initrd too large"};
    }
    memcpy(guest_mem + initrd_addr, data, static_cast<std::size_t>(size));
    boot_params_.ramdisk_image = initrd_addr;
    boot_params_.ramdisk_size = initrd_size;
    return {VmErrorCode::SUCCESS, "Initrd loaded"};
}

void BootLoader::add_e820_entry(uint64_t addr, uint64_t size, uint32_t type) {
    e820_map_.push_back({addr, size, type});
}

VmResult BootLoader::parse_bzimage_header(const std::vector<uint8_t>& data,
                                          BootParams& params) {
    if (data.size() < 0x268) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Image too small for boot header"};
    }

    memcpy(&params.setup_sects, data.data() + 0x1F1, 1);
    memcpy(&params.boot_flag, data.data() + 0x1FE, 2);
    memcpy(&params.header_magic, data.data() + 0x202, 4);
    memcpy(&params.header_version, data.data() + 0x206, 4);
    memcpy(&params.kernel_alignment, data.data() + 0x230, 4);
    memcpy(&params.relocatable_kernel, data.data() + 0x234, 1);
    memcpy(&params.xloadflags, data.data() + 0x236, 2);
    memcpy(&params.cmdline_size, data.data() + 0x238, 4);
    memcpy(&params.initrd_addr_max, data.data() + 0x22C, 4);
    memcpy(&params.payload_offset, data.data() + 0x248, 4);
    memcpy(&params.payload_length, data.data() + 0x24C, 4);
    memcpy(&params.pref_address, data.data() + 0x258, 8);
    memcpy(&params.init_size, data.data() + 0x260, 4);
    memcpy(&params.handover_offset, data.data() + 0x264, 4);

    if (params.boot_flag != 0xAA55) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid boot flag"};
    }
    if (params.header_magic != 0x53726448) {
        return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid header magic"};
    }

    return {VmErrorCode::SUCCESS, "bzImage header parsed"};
}

} // namespace domvirt::boot
