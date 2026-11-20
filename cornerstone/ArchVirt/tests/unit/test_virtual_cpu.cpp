#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "hypervisor/memory_manager.hpp"
#include "virtual/virtual_cpu.hpp"

using namespace domvirt;

TEST(VirtualCpuTest, ByteWritesPreserveUpperBitsAndCarryPropagates) {
    hypervisor::MemoryManager memory;
    ASSERT_TRUE(memory.allocate_ram(4096, 0, false));
    const std::uint8_t program[] = {
        0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // movabs rax, ...
        0xb0, 0xaa, // mov al, 0xaa
        0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff, // mov rcx, -1
        0x48, 0x83, 0xc1, 0x01, // add rcx, 1 -> carry
        0x48, 0x83, 0xd1, 0x00, // adc rcx, 0 -> 1
        0xf4,
    };
    std::memcpy(memory.translate(0), program, sizeof(program));
    virtualization::VirtualCpu cpu(memory, {0, 4096});
    ASSERT_TRUE(cpu.initialize());
    for (unsigned i = 0; i < 6; ++i) ASSERT_TRUE(cpu.step());
    EXPECT_EQ(cpu.registers().rax, 0x11223344556677aaull);
    EXPECT_EQ(cpu.registers().rcx, 1u);
    EXPECT_TRUE(cpu.halted());
}

TEST(VirtualCpuTest, DecodesAndExecutesIntegerInstructionsWithoutKvm) {
    hypervisor::MemoryManager memory;
    ASSERT_TRUE(memory.allocate_ram(2 * 1024 * 1024, 0, false));
    auto* guest = memory.translate(0);
    ASSERT_NE(guest, nullptr);

    // mov eax, 42; add eax, 1; hlt
    const std::uint8_t program[] = {
        0xB8, 0x2A, 0x00, 0x00, 0x00,
        0x83, 0xC0, 0x01,
        0xF4,
    };
    std::memcpy(guest + 0x1000, program, sizeof(program));

    virtualization::VirtualCpu cpu(
        memory,
        virtualization::VirtualCpuConfig{0x1000, 0x180000});
    ASSERT_TRUE(cpu.initialize());
    ASSERT_TRUE(cpu.step());
    ASSERT_TRUE(cpu.step());
    ASSERT_TRUE(cpu.step());

    EXPECT_EQ(cpu.registers().rax, 43u);
    EXPECT_TRUE(cpu.halted());
    EXPECT_EQ(cpu.instruction_count(), 3u);
}

TEST(VirtualCpuTest, RoutesSerialInputAndOutputThroughIoHandler) {
    hypervisor::MemoryManager memory;
    ASSERT_TRUE(memory.allocate_ram(2 * 1024 * 1024, 0, false));
    auto* guest = memory.translate(0);
    ASSERT_NE(guest, nullptr);

    // mov dx, 0x3f8; in al, dx; out dx, al; hlt
    const std::uint8_t program[] = {
        0x66, 0xBA, 0xF8, 0x03,
        0xEC,
        0xEE,
        0xF4,
    };
    std::memcpy(guest + 0x1000, program, sizeof(program));

    std::uint8_t output = 0;
    virtualization::VirtualCpu cpu(
        memory,
        virtualization::VirtualCpuConfig{0x1000, 0x180000});
    cpu.set_io_handler([&](std::uint16_t port, bool is_write,
                           std::uint8_t size, std::uint8_t value) {
        EXPECT_EQ(port, 0x3F8);
        EXPECT_EQ(size, 1u);
        if (is_write) {
            output = value;
            return static_cast<std::uint8_t>(0);
        }
        return static_cast<std::uint8_t>('u');
    });
    ASSERT_TRUE(cpu.initialize());
    ASSERT_TRUE(cpu.step());
    ASSERT_TRUE(cpu.step());
    ASSERT_TRUE(cpu.step());
    ASSERT_TRUE(cpu.step());

    EXPECT_EQ(cpu.registers().rax & 0xFFu, static_cast<std::uint64_t>('u'));
    EXPECT_EQ(output, static_cast<std::uint8_t>('u'));
}

TEST(VirtualCpuTest, CopiesRepeatedStringBytes) {
    hypervisor::MemoryManager memory;
    ASSERT_TRUE(memory.allocate_ram(2 * 1024 * 1024, 0, false));
    auto* guest = memory.translate(0);
    ASSERT_NE(guest, nullptr);

    // mov rsi, 0x2000; mov rdi, 0x2100; mov rcx, 3; rep movsb; hlt
    const std::uint8_t program[] = {
        0x48, 0xBE, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0xBF, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0xC7, 0xC1, 0x03, 0x00, 0x00, 0x00,
        0xF3, 0xA4,
        0xF4,
    };
    const std::uint8_t source[] = {'D', 'o', 'm'};
    std::memcpy(guest + 0x1000, program, sizeof(program));
    std::memcpy(guest + 0x2000, source, sizeof(source));

    virtualization::VirtualCpu cpu(
        memory,
        virtualization::VirtualCpuConfig{0x1000, 0x180000});
    ASSERT_TRUE(cpu.initialize());
    for (unsigned i = 0; i < 5; ++i) {
        ASSERT_TRUE(cpu.step());
    }

    EXPECT_EQ(std::memcmp(guest + 0x2100, source, sizeof(source)), 0);
    EXPECT_EQ(cpu.registers().rcx, 0u);
    EXPECT_TRUE(cpu.halted());
}
