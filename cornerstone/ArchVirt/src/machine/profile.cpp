#include "profile.hpp"
#include "machine_sources.hpp"

namespace domvirt::machine {

Profile Profile::load(std::string_view name, const std::string& path) {
    Architecture arch;
    if (name == "x86_64" || name == "x86-64" || name == "amd64") arch = Architecture::x86_64;
    else if (name == "aarch64" || name == "arm64") arch = Architecture::aarch64;
    else if (name == "riscv64" || name == "riscv" || name == "rv64") arch = Architecture::riscv64;
    else throw std::invalid_argument("Unsupported machine: " + std::string(name) +
                                     "; supported machines: x86_64, aarch64, riscv64");
    auto doc = path.empty() ? sandstone::archml::Document::parse(
        arch == Architecture::x86_64 ? sources::x86_64 :
        arch == Architecture::aarch64 ? sources::aarch64 : sources::riscv64, std::string(name))
        : sandstone::archml::Document::load(path);
    const auto* machine = doc.find("machine", "archvirt-pc");
    const std::string expected = arch == Architecture::x86_64 ? "amd64" :
                                 arch == Architecture::aarch64 ? "arm64" : "riscv64";
    const auto* target = doc.find("target", expected);
    auto property = [](const sandstone::archml::Node* n, std::string_view key) -> std::string {
        const auto* p = n ? n->property(key) : nullptr;
        if (!p || p->children.size() != 2)
            throw std::invalid_argument("Machine profile is missing " + std::string(key));
        return p->children[1].text;
    };
    if (!machine || !target || doc.name() != expected || property(target, "bits") != "64" ||
        property(target, "endian") != "little" || property(machine, "isa") != expected ||
        property(machine, "kind") != "personal-computer" || property(machine, "boot") != "domios-v1")
        throw std::invalid_argument("ArchML profile is not a supported DomIOS PC machine");
    auto number = [&](std::string_view key) {
        const auto text = property(machine, key);
        std::size_t consumed = 0;
        if (text.empty() || text[0] == '-') throw std::invalid_argument("Invalid machine address");
        auto value = std::stoull(text, &consumed, 0);
        if (consumed != text.size()) throw std::invalid_argument("Invalid machine address");
        return value;
    };
    // The bundled guest ABI is versioned and fixed; an ISA name alone cannot opt
    // a GPU, bytecode target, or incompatible board into this platform.
    if (number("elf-machine") != static_cast<unsigned>(arch) || number("ram-base") != 0 ||
        number("kernel-base") != 0x200000 || number("stack-top") != 0x700000 ||
        number("boot-info") != 0x10000 || number("initrd-base") != 0x800000 ||
        number("firmware-base") != 0x2000000 || number("console-base") != 0xb8000 ||
        number("console-columns") != 80 || number("console-rows") != 25 ||
        number("serial-base") != (arch == Architecture::x86_64 ? 0x3f8 : 0x10000000) ||
        property(machine, "serial-kind") != (arch == Architecture::x86_64 ? "port" : "mmio") ||
        property(machine, "interrupt-controller") != (arch == Architecture::x86_64 ? "apic" : "polled"))
        throw std::invalid_argument("Unsupported DomIOS machine layout or device contract");
    if (arch == Architecture::riscv64 &&
        (property(machine, "execution-isa") != "rv64im_zifencei" || property(machine, "halt") != "ebreak"))
        throw std::invalid_argument("Unsupported RISC-V execution environment");
    return {arch, arch == Architecture::x86_64 ? "x86_64" :
                  arch == Architecture::aarch64 ? "aarch64" : "riscv64", std::move(doc),
            0x200000, 0x700000, 0x10000, 0x800000, 0x2000000, 0xb8000,
            arch == Architecture::x86_64 ? 0x3f8u : 0x10000000u, 80, 25};
}

bool Profile::native_kvm() const noexcept {
#if defined(__x86_64__)
    return architecture == Architecture::x86_64;
#else
    return false;
#endif
}

} // namespace domvirt::machine
