#include "acpi.hpp"
#include <cstring>
#include <algorithm>

namespace domvirt::boot {

AcpiBuilder::AcpiBuilder() = default;

VmResult AcpiBuilder::build(uint8_t* guest_mem, uint64_t base_addr,
                            int num_cpus, uint64_t mem_size) {
    if (!guest_mem || base_addr >= mem_size) {
        return {VmErrorCode::ACPI_BUILD_FAILED, "ACPI placement is outside guest memory"};
    }
    num_cpus_ = num_cpus > 0 ? num_cpus : 1;
    mem_size_ = mem_size;

    rsdp_addr_ = base_addr;
    madt_addr_ = base_addr + RSDP_SIZE;
    fadt_addr_ = base_addr + RSDP_SIZE + MADT_ENTRY_SIZE * num_cpus_ + 44;
    dsdt_addr_ = base_addr + RSDP_SIZE + MADT_ENTRY_SIZE * num_cpus_ + 44 + FADT_SIZE;
    xsdt_addr_ = dsdt_addr_ + DSDT_HEADER_SIZE + 36;
    const uint64_t end_addr = xsdt_addr_ + 36 + 3 * sizeof(uint64_t);
    if (end_addr > mem_size || end_addr < xsdt_addr_) {
        return {VmErrorCode::ACPI_BUILD_FAILED, "ACPI tables do not fit guest memory"};
    }

    write_rsdp(guest_mem, rsdp_addr_);
    write_madt(guest_mem, madt_addr_, num_cpus_);
    write_fadt(guest_mem, fadt_addr_);
    write_dsdt(guest_mem, dsdt_addr_);
    write_xsdt(guest_mem, xsdt_addr_);

    return {VmErrorCode::SUCCESS, "ACPI tables built"};
}

void AcpiBuilder::set_pci_bus_range(uint8_t start, uint8_t end) {
    pci_bus_start_ = start;
    pci_bus_end_ = end;
}

void AcpiBuilder::write_rsdp(uint8_t* mem, uint64_t addr) {
    memset(mem + addr, 0, RSDP_SIZE);
    memcpy(mem + addr, "RSD PTR ", 8);
    mem[addr + 15] = 2;
    memcpy(mem + addr + 16, &xsdt_addr_, 8);
    mem[addr + 8] = 'D';
    mem[addr + 9] = 'O';
    mem[addr + 10] = 'M';
    mem[addr + 11] = 'V';
    mem[addr + 12] = 'I';
    mem[addr + 13] = 'R';
    mem[addr + 14] = 'T';

    uint8_t checksum = 0;
    for (int i = 0; i < 20; ++i) checksum += mem[addr + i];
    mem[addr + 8] -= checksum;
}

void AcpiBuilder::write_madt(uint8_t* mem, uint64_t addr, int num_cpus) {
    uint32_t length = 44 + num_cpus * MADT_ENTRY_SIZE;
    write_table_header(mem, addr, "APIC", length, 3);

    memcpy(mem + addr + 36, &io_apic_base_, 4);
    mem[addr + 40] = 0x01;

    for (int i = 0; i < num_cpus; ++i) {
        uint64_t entry_addr = addr + 44 + i * MADT_ENTRY_SIZE;
        mem[entry_addr] = 0;
        mem[entry_addr + 1] = MADT_ENTRY_SIZE;
        mem[entry_addr + 2] = static_cast<uint8_t>(i);
        mem[entry_addr + 3] = static_cast<uint8_t>(i);
        uint32_t flags = 1;
        memcpy(mem + entry_addr + 4, &flags, 4);
    }
}

void AcpiBuilder::write_fadt(uint8_t* mem, uint64_t addr) {
    write_table_header(mem, addr, "FACP", FADT_SIZE, 3);

    memcpy(mem + addr + 36, &dsdt_addr_, 4);
    mem[addr + 40] = 0;
    mem[addr + 46] = 0x00;
    mem[addr + 47] = 0x09;
    mem[addr + 48] = 0x20;
    mem[addr + 49] = 0x02;
    mem[addr + 50] = 0x00;
    mem[addr + 51] = 0x00;
    mem[addr + 52] = 0x80;
    mem[addr + 53] = 0x08;
    mem[addr + 54] = 0x00;
    mem[addr + 55] = 0x00;
}

void AcpiBuilder::write_dsdt(uint8_t* mem, uint64_t addr) {
    uint32_t length = DSDT_HEADER_SIZE + 36;
    write_table_header(mem, addr, "DSDT", length, 2);

    mem[addr + 36] = 0x10;
    mem[addr + 37] = static_cast<uint8_t>(length - 38);
    mem[addr + 38] = 0x5C;
    mem[addr + 39] = 0x00;
    mem[addr + 40] = 0x5B;
    mem[addr + 41] = 0x80;
    mem[addr + 42] = 0x44;
    mem[addr + 43] = 0x42;
    mem[addr + 44] = 0x47;
    mem[addr + 45] = 0x5F;
    mem[addr + 46] = 0x01;
    mem[addr + 47] = 0x0A;
    mem[addr + 48] = 0x01;
    mem[addr + 49] = 0x00;
    mem[addr + 50] = 0x0A;
    mem[addr + 51] = 0x02;
    mem[addr + 52] = 0x5B;
    mem[addr + 53] = 0x82;
    mem[addr + 54] = 0x44;
    mem[addr + 55] = 0x42;
    mem[addr + 56] = 0x47;
    mem[addr + 57] = 0x30;
    mem[addr + 58] = 0x08;
    mem[addr + 59] = 0x5F;
    mem[addr + 60] = 0x48;
    mem[addr + 61] = 0x49;
    mem[addr + 62] = 0x44;
    mem[addr + 63] = 0x00;
    mem[addr + 64] = 0x14;
    mem[addr + 65] = 0x09;
    mem[addr + 66] = 0x5F;
    mem[addr + 67] = 0x53;
    mem[addr + 68] = 0x54;
    mem[addr + 69] = 0x41;
    mem[addr + 70] = 0xA4;
    mem[addr + 71] = 0x00;
}

void AcpiBuilder::write_xsdt(uint8_t* mem, uint64_t addr) {
    constexpr uint32_t length = 36 + 3 * sizeof(uint64_t);
    write_table_header(mem, addr, "XSDT", length, 1);
    std::memcpy(mem + addr + 36, &madt_addr_, sizeof(madt_addr_));
    std::memcpy(mem + addr + 44, &fadt_addr_, sizeof(fadt_addr_));
    std::memcpy(mem + addr + 52, &dsdt_addr_, sizeof(dsdt_addr_));
}

void AcpiBuilder::write_table_header(uint8_t* mem, uint64_t addr,
                                     const char sig[4], uint32_t length,
                                     uint8_t revision) {
    memset(mem + addr, 0, length);
    memcpy(mem + addr, sig, 4);
    memcpy(mem + addr + 4, &length, 4);
    mem[addr + 8] = revision;
    mem[addr + 9] = 0;
    memcpy(mem + addr + 10, "DOMVIRT", 6);
    memcpy(mem + addr + 16, "DOMVIRT", 8);
    const uint32_t revision32 = revision;
    memcpy(mem + addr + 24, &revision32, 4);
    memcpy(mem + addr + 28, &revision32, 4);

    uint8_t checksum = 0;
    for (uint32_t i = 0; i < length; ++i) checksum += mem[addr + i];
    mem[addr + 9] = static_cast<uint8_t>(256 - checksum);
}

} // namespace domvirt::boot
