#pragma once
#include <ArchML.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace domvirt::machine {

enum class Architecture : std::uint16_t { x86_64 = 62, aarch64 = 183, riscv64 = 243 };

struct Profile {
    Architecture architecture;
    std::string name;
    sandstone::archml::Document isa;
    std::uint64_t kernel_base, stack_top, boot_info, initrd_base, firmware_base;
    std::uint64_t console_base, serial_base;
    unsigned console_columns, console_rows;

    static Profile load(std::string_view machine = "x86_64", const std::string& spec_path = {});
    bool native_kvm() const noexcept;
};

} // namespace domvirt::machine
