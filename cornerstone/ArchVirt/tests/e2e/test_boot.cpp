#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "boot/loader.hpp"
#include "boot/acpi.hpp"

using namespace domvirt::boot;

TEST(BootLoaderTest, InvalidFile) {
    BootLoader loader;
    uint64_t entry = 0;
    auto result = loader.load_bzimage("/nonexistent", nullptr, 0, entry);
    EXPECT_FALSE(result);
}

TEST(BootLoaderTest, EmptyE820) {
    BootLoader loader;
    EXPECT_TRUE(loader.e820_map().empty());
    loader.add_e820_entry(0, 0x100000, 1);
    EXPECT_EQ(loader.e820_map().size(), 1u);
}

TEST(BootLoaderTest, DefaultBootParams) {
    BootLoader loader;
    auto& params = loader.boot_params();
    EXPECT_EQ(params.boot_flag, 0xAA55);
    EXPECT_EQ(params.header_magic, 0x53726448);
}

TEST(AcpiBuilderTest, Build) {
    AcpiBuilder builder;
    std::vector<uint8_t> mem(1024 * 1024);
    auto result = builder.build(mem.data(), 0, 2, 128 * 1024 * 1024);
    EXPECT_TRUE(result);
    EXPECT_GT(builder.rsdp_addr(), 0u);
    EXPECT_GT(builder.madt_addr(), 0u);
    EXPECT_GT(builder.fadt_addr(), 0u);
    EXPECT_GT(builder.dsdt_addr(), 0u);
}

TEST(AcpiBuilderTest, RSDPMagic) {
    AcpiBuilder builder;
    std::vector<uint8_t> mem(1024 * 1024);
    builder.build(mem.data(), 0, 1, 64 * 1024 * 1024);
    std::string magic(reinterpret_cast<char*>(&mem[0]), 8);
    EXPECT_EQ(magic, "RSD PTR ");
}
