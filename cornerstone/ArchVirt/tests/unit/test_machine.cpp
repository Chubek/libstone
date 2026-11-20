#include <gtest/gtest.h>
#include "machine/profile.hpp"
#include "virtual/aarch64_cpu.hpp"
#include "boot/loader.hpp"
#include "firmware.hpp"
#include "dit.hpp"
#include "lua_runtime.hpp"
#include <cstring>

using namespace domvirt;

TEST(ArchMLMachine, RestrictsProfilesAndDecodesStandardInstructions) {
    EXPECT_THROW(machine::Profile::load("wasm"), std::invalid_argument);
    auto profile = machine::Profile::load("arm64");
    EXPECT_EQ(profile.architecture, machine::Architecture::aarch64);
    // ADD X0, X1, X2, followed by an unallocated encoding.
    const std::uint8_t add[] = {0x20, 0x00, 0x02, 0x8b};
    sandstone::archml::runtime::Decoded decoded;
    EXPECT_TRUE(profile.isa.decode(add, decoded));
    EXPECT_EQ(decoded.size, 4u);
    const std::uint8_t invalid[] = {0xff, 0xff, 0xff, 0xff};
    EXPECT_FALSE(profile.isa.decode(invalid, decoded));
}

TEST(Aarch64Machine, FlagsZeroRegisterAndFaultRollback) {
    auto profile = machine::Profile::load("aarch64");
    hypervisor::MemoryManager memory;
    ASSERT_TRUE(memory.allocate_ram(4096, 0, false));
    // ADDS X0,X1,X2; ADDS XZR,X1,X2; LDR X0,[X1]; HLT #0
    const std::uint32_t code[] = {0xab020020, 0xab02003f, 0xf9400020, 0xd4400000};
    std::memcpy(memory.translate(0), code, sizeof(code));
    virtualization::Aarch64Cpu cpu(memory, profile, 0, 0, {});
    auto state = cpu.state(); state.x[1] = UINT64_MAX; state.x[2] = 1;
    cpu.set_state(state);
    ASSERT_TRUE(cpu.step());
    EXPECT_EQ(cpu.state().x[0], 0u);
    EXPECT_EQ(cpu.state().nzcv, 0x60000000u); // Z and carry
    ASSERT_TRUE(cpu.step());
    EXPECT_EQ(cpu.state().sp, profile.stack_top);
    auto result = cpu.step();
    EXPECT_FALSE(result);
    EXPECT_EQ(cpu.state().pc, 8u);
    EXPECT_EQ(cpu.state().x[0], 0u);
    state = cpu.state(); state.pc = 12; cpu.set_state(state);
    ASSERT_TRUE(cpu.step()); EXPECT_TRUE(cpu.halted());
}

TEST(DomIOSFirmware, ChecksSectionBoundsAndCorruption) {
    auto dit = domios::serialize_dit(domios::make_default_dit());
    std::vector<domios::FirmwarePart> parts{
        {domios::FirmwareSection::dit, {dit.bytes.begin(), dit.bytes.end()}},
        {domios::FirmwareSection::logo, {'L'}},
        {domios::FirmwareSection::background, {'B'}},
        {domios::FirmwareSection::lua, {'L'}},
        {domios::FirmwareSection::manifest, {'M'}}};
    auto image = domios::pack_firmware(parts);
    std::vector<domios::FirmwarePart> read;
    std::string error;
    ASSERT_TRUE(domios::unpack_firmware(image, read, error)) << error;
    ASSERT_EQ(read.size(), parts.size());
    image.back() ^= 1;
    EXPECT_FALSE(domios::unpack_firmware(image, read, error));
    image.back() ^= 1;
    image[24] = 0; // first section points inside header
    EXPECT_FALSE(domios::unpack_firmware(image, read, error));
}

TEST(DomIOSLua, RunsArgumentsAndEnforcesExecutionBudget) {
    domios::LiosRuntime lua;
    domios::EventQueue events(16);
    domios::Config config;
    std::string error;
    ASSERT_TRUE(lua.initialize(config, events, error));
    ASSERT_TRUE(lua.load_embedded_program("test", "return string.upper(arg[1])"));
    EXPECT_EQ(lua.run_program("test", {"guest"}, events), "GUEST");
    ASSERT_TRUE(lua.load_embedded_program("sandbox", "return os == nil and io == nil and package == nil and debug == nil"));
    EXPECT_EQ(lua.run_program("sandbox", {}, events), "true");
    ASSERT_TRUE(lua.load_embedded_program("loop", "while true do end"));
    EXPECT_NE(lua.run_program("loop", {}, events).find("budget exceeded"), std::string::npos);
    EXPECT_EQ(lua.run_program("test", {"again"}, events), "AGAIN");
}

TEST(ElfLoader, ValidatesBeforeWritingAndZerosBss) {
    std::vector<std::uint8_t> elf(128), memory(4096, 0xa5);
    auto put = [&](unsigned at, std::uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) elf[at + i] = value >> (i * 8);
    };
    std::memcpy(elf.data(), "\177ELF\2\1\1", 7);
    put(16, 2, 2); put(18, 183, 2); put(20, 1, 4);
    put(24, 0x100, 8); put(32, 64, 8); put(52, 64, 2); put(54, 56, 2); put(56, 1, 2);
    put(64, 1, 4); put(68, 5, 4); put(72, 120, 8);
    put(80, 0x100, 8); put(88, 0x100, 8); put(96, 4, 8); put(104, 16, 8);
    elf[120] = 42;
    boot::BootLoader loader;
    std::uint64_t entry = 0;
    EXPECT_FALSE(loader.load_elf(elf, memory.data(), memory.size(), entry, 62));
    EXPECT_EQ(memory[0x100], 0xa5);
    put(104, 2, 8); // file size > memory size
    EXPECT_FALSE(loader.load_elf(elf, memory.data(), memory.size(), entry, 183));
    EXPECT_EQ(memory[0x100], 0xa5);
    put(104, 16, 8);
    ASSERT_TRUE(loader.load_elf(elf, memory.data(), memory.size(), entry, 183));
    EXPECT_EQ(entry, 0x100u); EXPECT_EQ(memory[0x100], 42);
    EXPECT_EQ(memory[0x10f], 0); EXPECT_EQ(memory[0x110], 0xa5);
}
