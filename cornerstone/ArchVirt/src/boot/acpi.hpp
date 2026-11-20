#pragma once
#include <cstdint>
#include <vector>
#include "domvirt_types.hpp"

namespace domvirt::boot {

struct MadtEntry {
    uint8_t type;
    uint8_t length;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
};

class AcpiBuilder {
public:
    AcpiBuilder();
    ~AcpiBuilder() = default;

    VmResult build(uint8_t* guest_mem, uint64_t base_addr,
                   int num_cpus, uint64_t mem_size);

    void set_pci_bus_range(uint8_t start, uint8_t end);
    void set_io_apic_base(uint64_t base) { io_apic_base_ = base; }

    [[nodiscard]] uint64_t rsdp_addr() const noexcept { return rsdp_addr_ ? rsdp_addr_ : 1; }
    [[nodiscard]] uint64_t madt_addr() const noexcept { return madt_addr_ ? madt_addr_ : 1; }
    [[nodiscard]] uint64_t fadt_addr() const noexcept { return fadt_addr_ ? fadt_addr_ : 1; }
    [[nodiscard]] uint64_t dsdt_addr() const noexcept { return dsdt_addr_ ? dsdt_addr_ : 1; }
    [[nodiscard]] uint64_t xsdt_addr() const noexcept { return xsdt_addr_; }

    static constexpr size_t RSDP_SIZE = 36;
    static constexpr size_t MADT_ENTRY_SIZE = 8;
    static constexpr size_t FADT_SIZE = 276;
    static constexpr size_t DSDT_HEADER_SIZE = 36;

private:
    void write_rsdp(uint8_t* mem, uint64_t addr);
    void write_madt(uint8_t* mem, uint64_t addr, int num_cpus);
    void write_fadt(uint8_t* mem, uint64_t addr);
    void write_dsdt(uint8_t* mem, uint64_t addr);
    void write_xsdt(uint8_t* mem, uint64_t addr);
    void write_table_header(uint8_t* mem, uint64_t addr,
                            const char sig[4], uint32_t length,
                            uint8_t revision);

    uint64_t rsdp_addr_{0};
    uint64_t madt_addr_{0};
    uint64_t fadt_addr_{0};
    uint64_t dsdt_addr_{0};
    uint64_t xsdt_addr_{0};
    uint64_t io_apic_base_{0xFEC00000};
    uint8_t pci_bus_start_{0};
    uint8_t pci_bus_end_{0xFF};
    int num_cpus_{1};
    uint64_t mem_size_{0};
};

} // namespace domvirt::boot
