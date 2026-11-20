#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cstddef>

#include "boot/acpi.hpp"
#include "boot/loader.hpp"
#include "hypervisor/memory_manager.hpp"
#include "test_helpers.hpp"

using namespace domvirt::boot;
using namespace domvirt::hypervisor;
using namespace domvirt::test_support;

TEST(BootMemoryTest, AllocateRamRejectsZero) {
    MemoryManager mm;
    EXPECT_FALSE(mm.allocate_ram(0, 0, false));
}

TEST(BootMemoryTest, AllocateRamTracksSize) {
    MemoryManager mm;
    EXPECT_TRUE(mm.allocate_ram(2 * 1024 * 1024, 0, false));
    EXPECT_GE(mm.total_ram(), 2u * 1024 * 1024);
}

TEST(BootMemoryTest, TranslateRoundTripWorks) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(2 * 1024 * 1024, 0, false));
    auto* ptr = mm.translate(0x1000);
    ASSERT_NE(ptr, nullptr);
    ptr[0] = 0xAB;
    EXPECT_EQ(mm.translate(0x1000)[0], 0xAB);
}

TEST(BootMemoryTest, GetSpanReturnsRequestedLength) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(2 * 1024 * 1024, 0, false));
    auto span = mm.get_span(0x2000, 16);
    ASSERT_EQ(span.size(), 16u);
    span[0] = 0x11;
    EXPECT_EQ(mm.translate(0x2000)[0], 0x11);
}

TEST(BootMemoryTest, GpaFromHostRoundTrips) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(2 * 1024 * 1024, 0, false));
    auto* host = mm.translate(0x3000);
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(mm.gpa_from_host(host), 0x3000u);
}

TEST(BootMemoryTest, MoveConstructorPreservesMapping) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(2 * 1024 * 1024, 0, false));
    auto* host = mm.translate(0x4000);
    ASSERT_NE(host, nullptr);
    host[0] = 0x33;
    MemoryManager moved(std::move(mm));
    auto* moved_host = moved.translate(0x4000);
    ASSERT_NE(moved_host, nullptr);
    EXPECT_EQ(moved_host[0], 0x33);
}

TEST(BootMemoryTest, IdentityPagingSucceeds) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(16 * 1024 * 1024, 0, false));
    EXPECT_TRUE(mm.setup_identity_paging());
}

TEST(BootMemoryTest, IdentityPagingFailsOnTinyMemory) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(8 * 1024 * 1024, 0, false));
    EXPECT_FALSE(mm.setup_identity_paging(8 * 1024 * 1024 - 0x2000));
}

TEST(BootMemoryTest, DefaultBootParamsAreSeeded) {
    BootLoader loader;
    const auto& params = loader.boot_params();
    EXPECT_EQ(params.boot_flag, 0xAA55);
    EXPECT_EQ(params.header_magic, 0x53726448u);
    EXPECT_EQ(params.cmdline_size, 2048u);
}

TEST(BootMemoryTest, AddE820EntryAppends) {
    BootLoader loader;
    loader.add_e820_entry(0, 0x1000, 1);
    EXPECT_EQ(loader.e820_map().size(), 1u);
    EXPECT_EQ(loader.e820_map().front().addr, 0u);
}

TEST(BootMemoryTest, WriteBootParamsWritesCmdlinePointer) {
    BootLoader loader;
    loader.set_cmdline("console=ttyS0");
    std::vector<std::uint8_t> mem(1 << 20, 0);
    ASSERT_TRUE(loader.write_boot_params(mem.data()));
    EXPECT_EQ(read_u32(mem, BootLoader::BOOT_PARAMS_BASE + offsetof(BootParams, cmd_line_ptr)),
              BootLoader::CMDLINE_BASE);
    std::string cmd(reinterpret_cast<char*>(mem.data() + BootLoader::CMDLINE_BASE));
    EXPECT_EQ(cmd, "console=ttyS0");
}

TEST(BootMemoryTest, WriteBootParamsSeedsE820Count) {
    BootLoader loader;
    std::vector<std::uint8_t> mem(1 << 20, 0);
    ASSERT_TRUE(loader.write_boot_params(mem.data()));
    EXPECT_EQ(read_u32(mem, BootLoader::BOOT_PARAMS_BASE + 0x2D0), 4u);
}

TEST(BootMemoryTest, LoadBlobCopiesBytes) {
    BootLoader loader;
    std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
    auto path = write_temp_bytes("blob", payload);
    std::vector<std::uint8_t> mem(1 << 20, 0);
    std::uint64_t size = 0;
    ASSERT_TRUE(loader.load_blob(path.string(), mem.data(), mem.size(), 0x1000, size));
    EXPECT_EQ(size, payload.size());
    EXPECT_EQ(mem[0x1000], 1);
    EXPECT_EQ(mem[0x1004], 5);
}

TEST(BootMemoryTest, LoadBlobRejectsMissingFile) {
    BootLoader loader;
    std::vector<std::uint8_t> mem(1 << 20, 0);
    std::uint64_t size = 0;
    EXPECT_FALSE(loader.load_blob("/no/such/file", mem.data(), mem.size(), 0x1000, size));
}

TEST(BootMemoryTest, LoadBlobRejectsOutOfBoundsPlacement) {
    BootLoader loader;
    std::vector<std::uint8_t> payload{1, 2, 3};
    auto path = write_temp_bytes("blob", payload);
    std::vector<std::uint8_t> mem(64, 0);
    std::uint64_t size = 0;
    EXPECT_FALSE(loader.load_blob(path.string(), mem.data(), mem.size(), 63, size));
}

TEST(BootMemoryTest, LoadInitrdWritesRamdiskFields) {
    BootLoader loader;
    std::vector<std::uint8_t> payload{9, 8, 7, 6};
    auto path = write_temp_bytes("initrd", payload);
    std::vector<std::uint8_t> mem(16 * 1024 * 1024, 0);
    std::uint64_t addr = 0, size = 0;
    ASSERT_TRUE(loader.load_initrd(path.string(), mem.data(), mem.size(), addr, size));
    EXPECT_EQ(addr, 0x800000u);
    EXPECT_EQ(size, payload.size());
    EXPECT_EQ(loader.boot_params().ramdisk_image, 0x800000u);
    EXPECT_EQ(loader.boot_params().ramdisk_size, payload.size());
}

TEST(BootMemoryTest, AcpiBuildWritesRsdpmagic) {
    AcpiBuilder builder;
    std::vector<std::uint8_t> mem(1 << 20, 0);
    ASSERT_TRUE(builder.build(mem.data(), 0x1000, 1, 16 * 1024 * 1024));
    EXPECT_EQ(std::string(reinterpret_cast<char*>(mem.data() + 0x1000), 8), "RSD PTR ");
}

TEST(BootMemoryTest, AcpiAddressesArePopulated) {
    AcpiBuilder builder;
    std::vector<std::uint8_t> mem(1 << 20, 0);
    ASSERT_TRUE(builder.build(mem.data(), 0x2000, 2, 16 * 1024 * 1024));
    EXPECT_GT(builder.rsdp_addr(), 0u);
    EXPECT_GT(builder.madt_addr(), 0u);
    EXPECT_GT(builder.fadt_addr(), 0u);
    EXPECT_GT(builder.dsdt_addr(), 0u);
    EXPECT_GT(builder.xsdt_addr(), 0u);
}
