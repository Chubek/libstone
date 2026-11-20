#include <gtest/gtest.h>
#include "virtual/riscv64_cpu.hpp"
#include <bit>
#include <random>

using namespace domvirt;
using U = std::uint64_t;
using S = std::int64_t;
__extension__ typedef unsigned __int128 W;
__extension__ typedef __int128 SW;

namespace {
std::uint32_t regop(unsigned f3, unsigned f7 = 0, unsigned op = 0x33, unsigned rd = 5) {
    return (f7 << 25) | (7 << 20) | (6 << 15) | (f3 << 12) | (rd << 7) | op;
}
std::uint32_t immediate(unsigned f3, unsigned imm, unsigned op = 0x13, unsigned rd = 5) {
    return (imm << 20) | (6 << 15) | (f3 << 12) | (rd << 7) | op;
}
U word(U v) { return U(std::int64_t(std::bit_cast<std::int32_t>(std::uint32_t(v)))); }
class RiscvMachine : public testing::Test {
protected:
    machine::Profile profile = machine::Profile::load("riscv64");
    hypervisor::MemoryManager memory;
    std::unique_ptr<virtualization::Riscv64Cpu> cpu;
    std::vector<std::uint8_t> output;
    unsigned reads = 0;
    void SetUp() override {
        ASSERT_TRUE(memory.allocate_ram(4096, 0, false));
        cpu = std::make_unique<virtualization::Riscv64Cpu>(memory, profile, 0, 0x10000,
            [&](bool write, std::uint8_t v) { if (write) output.push_back(v); else ++reads; return std::uint8_t{0x80}; });
    }
    void instruction(std::uint32_t w) {
        for (unsigned i=0;i<4;++i) memory.translate(0)[i] = w >> (8*i);
    }
    VmResult run(std::uint32_t w, U a=0, U b=0) {
        auto state = cpu->state(); state.pc=0; state.x[6]=a; state.x[7]=b; state.x[5]=0xdeadbeef;
        cpu->set_state(state); instruction(w); return cpu->step();
    }
    U result() { return cpu->state().x[5]; }
};
}

TEST_F(RiscvMachine, BootZeroRegisterAndImmediateWidths) {
    EXPECT_EQ(profile.architecture, machine::Architecture::riscv64);
    EXPECT_FALSE(profile.native_kvm());
    EXPECT_EQ(machine::Profile::load("rv64").name, "riscv64");
    EXPECT_EQ(cpu->state().x[10], 0x10000u);
    EXPECT_EQ(cpu->state().x[2], 0x700000u);
    ASSERT_TRUE(run(immediate(0, 0xfff), 0)); EXPECT_EQ(result(), UINT64_MAX);
    ASSERT_TRUE(run(immediate(0, 1, 0x13, 0), UINT64_MAX)); EXPECT_EQ(cpu->state().x[0], 0u);
    ASSERT_TRUE(run(0x800002b7)); EXPECT_EQ(result(), 0xffffffff80000000ull); // LUI x5
    ASSERT_TRUE(run(0xfffff297)); EXPECT_EQ(result(), 0xfffffffffffff000ull); // AUIPC x5 at PC 0
    ASSERT_TRUE(run(immediate(0, 1, 0x1b), 0x7fffffff)); EXPECT_EQ(result(), 0xffffffff80000000ull);
    ASSERT_TRUE(run(immediate(3, 0xfff), UINT64_MAX - 1)); EXPECT_EQ(result(), 1u);
    ASSERT_TRUE(run(immediate(2, 0), UINT64_MAX)); EXPECT_EQ(result(), 1u);
}

TEST_F(RiscvMachine, IntegerAluAndMaskedShifts) {
    const U a = 0x87654321abcdef01, b = 0x1122334455667788;
    const U expected[] = {a+b, a << (b&63), 1, 0, a^b, a >> (b&63), a|b, a&b};
    for (unsigned f=0;f<8;++f) { ASSERT_TRUE(run(regop(f),a,b)); EXPECT_EQ(result(),expected[f]); }
    ASSERT_TRUE(run(regop(0,0x20),a,b)); EXPECT_EQ(result(),a-b);
    for (unsigned n : {0u,1u,31u,32u,63u,64u,127u}) {
        ASSERT_TRUE(run(regop(5,0x20),a,n)); EXPECT_EQ(result(),U(std::bit_cast<S>(a)>>(n&63)));
        ASSERT_TRUE(run(regop(1,0,0x3b),a,n)); EXPECT_EQ(result(),word(a<<(n&31)));
        ASSERT_TRUE(run(regop(5,0,0x3b),a,n)); EXPECT_EQ(result(),word(std::uint32_t(a)>>(n&31)));
        ASSERT_TRUE(run(regop(5,0x20,0x3b),a,n)); EXPECT_EQ(result(),word(U(std::bit_cast<S>(word(a))>>(n&31))));
    }
    ASSERT_TRUE(run(immediate(1,63),1)); EXPECT_EQ(result(),U{1}<<63);
    ASSERT_TRUE(run(immediate(5,0x43f),U{1}<<63)); EXPECT_EQ(result(),UINT64_MAX);
    ASSERT_TRUE(run(immediate(5,31,0x1b),0x80000000)); EXPECT_EQ(result(),1u);
    ASSERT_TRUE(run(immediate(5,0x41f,0x1b),0x80000000)); EXPECT_EQ(result(),UINT64_MAX);
    for (unsigned f : {4u,6u,7u}) {
        ASSERT_TRUE(run(immediate(f,0xf80),a));
        const U imm=U{0}-128;
        EXPECT_EQ(result(), f==4 ? a^imm : f==6 ? a|imm : a&imm);
    }
}

TEST_F(RiscvMachine, MultiplyHighSignednessAndWordResults) {
    std::mt19937_64 random(0x52563634);
    for (unsigned i=0;i<100;++i) {
        U a=random(),b=random();
        const U expected[] = {U(W(a)*b), U(W(SW(std::bit_cast<S>(a))*SW(std::bit_cast<S>(b)))>>64),
                              U(W(SW(std::bit_cast<S>(a))*SW(b))>>64), U((W(a)*b)>>64)};
        for (unsigned f=0;f<4;++f) { ASSERT_TRUE(run(regop(f,1),a,b)); EXPECT_EQ(result(),expected[f]); }
        ASSERT_TRUE(run(regop(0,1,0x3b),a,b)); EXPECT_EQ(result(),word(a*b));
    }
}

TEST_F(RiscvMachine, DivisionZeroOverflowAndRounding) {
    struct Row { U a,b,q,r; };
    for (const auto& row : {Row{U(-7),3,U(-2),U(-1)}, Row{7,U(-3),U(-2),1},
                           Row{U{1}<<63,UINT64_MAX,U{1}<<63,0}, Row{123,0,UINT64_MAX,123}}) {
        ASSERT_TRUE(run(regop(4,1),row.a,row.b)); EXPECT_EQ(result(),row.q);
        ASSERT_TRUE(run(regop(6,1),row.a,row.b)); EXPECT_EQ(result(),row.r);
    }
    ASSERT_TRUE(run(regop(5,1),UINT64_MAX,2)); EXPECT_EQ(result(),UINT64_MAX/2);
    ASSERT_TRUE(run(regop(7,1),UINT64_MAX,2)); EXPECT_EQ(result(),1u);
    ASSERT_TRUE(run(regop(5,1,0x3b),UINT64_MAX,1)); EXPECT_EQ(result(),UINT64_MAX);
    ASSERT_TRUE(run(regop(4,1,0x3b),0x80000000,UINT64_MAX)); EXPECT_EQ(result(),0xffffffff80000000ull);
    ASSERT_TRUE(run(regop(6,1,0x3b),0x80000000,UINT64_MAX)); EXPECT_EQ(result(),0u);
    for (unsigned f=4;f<8;++f) {
        ASSERT_TRUE(run(regop(f,1,0x3b),0x1234567887654321,0));
        EXPECT_EQ(result(), f<6 ? UINT64_MAX : 0xffffffff87654321ull);
    }
}

TEST_F(RiscvMachine, BranchesLinkAliasingAndPreciseTargetFaults) {
    // Six branch comparisons, displacement +8; no link register is written.
    for (unsigned f : {0u,1u,4u,5u,6u,7u}) {
        bool taken = f==1 || f==4 || f==7;
        ASSERT_TRUE(run((7<<20)|(6<<15)|(f<<12)|(4<<8)|0x63,UINT64_MAX,1));
        EXPECT_EQ(cpu->state().pc,taken?8u:4u);
        EXPECT_EQ(result(),0xdeadbeefu);
    }
    ASSERT_TRUE(run(0x008002ef)); EXPECT_EQ(result(),4u); EXPECT_EQ(cpu->state().pc,8u); // JAL x5,+8
    ASSERT_TRUE(run(immediate(0,1,0x67,6),8)); // JALR x6,x6,1: clear bit zero, read before write
    EXPECT_EQ(cpu->state().x[6],4u); EXPECT_EQ(cpu->state().pc,8u);
    EXPECT_FALSE(run(0x002002ef)); EXPECT_EQ(result(),0xdeadbeefu); EXPECT_EQ(cpu->state().pc,0u);
    EXPECT_FALSE(run(immediate(0,0,0x67),6)); EXPECT_EQ(cpu->state().pc,0u);
    // Untaken BEQ +2 is legal even though the potential target is misaligned.
    ASSERT_TRUE(run((7<<20)|(6<<15)|(1<<8)|0x63,1,2)); EXPECT_EQ(cpu->state().pc,4u);
}

TEST_F(RiscvMachine, LittleEndianMemorySignedLoadsAndFaultAtomicity) {
    for (unsigned f=0;f<=3;++f) {
        auto store=(7<<20)|(6<<15)|(f<<12)|0x23;
        ASSERT_TRUE(run(store,256,0xfedcba9876543280));
        for (unsigned i=0;i<(1u<<f);++i) EXPECT_EQ(memory.translate(256)[i],std::uint8_t(0xfedcba9876543280ull>>(8*i)));
        ASSERT_TRUE(run(immediate(f,0,0x03),256));
        const U expected[]={0xffffffffffffff80ull,0x3280,0x76543280,0xfedcba9876543280ull};
        EXPECT_EQ(result(),expected[f]);
    }
    ASSERT_TRUE(run(immediate(4,0,0x03),256)); EXPECT_EQ(result(),0x80u);
    ASSERT_TRUE(run(immediate(5,0,0x03),256)); EXPECT_EQ(result(),0x3280u);
    ASSERT_TRUE(run(immediate(6,0,0x03),256)); EXPECT_EQ(result(),0x76543280u);
    EXPECT_FALSE(run(immediate(3,0,0x03),4096)); EXPECT_EQ(result(),0xdeadbeefu);
    EXPECT_FALSE(run(immediate(3,0,0x03),UINT64_MAX-7)); EXPECT_EQ(cpu->state().pc,0u);
    EXPECT_FALSE(run((7<<20)|(6<<15)|(3<<12)|0x23,4093,0));
    EXPECT_EQ(memory.translate(4093)[0],0); // no partial write
    EXPECT_FALSE(run(immediate(1,0,0x03),257)); EXPECT_EQ(result(),0xdeadbeefu);
    EXPECT_FALSE(run(immediate(0,0,0x03,0),4096)); // x0 cannot suppress access faults
}

TEST_F(RiscvMachine, MmioFencesSelfModificationAndHalt) {
    ASSERT_TRUE(run(immediate(4,0,0x03,0),profile.serial_base)); EXPECT_EQ(reads,1u);
    ASSERT_TRUE(run((7<<20)|(6<<15)|0x23,profile.serial_base,0xab)); ASSERT_EQ(output.size(),1u); EXPECT_EQ(output[0],0xab);
    EXPECT_FALSE(run(immediate(2,0,0x03),profile.serial_base)); EXPECT_EQ(reads,1u);
    ASSERT_TRUE(run(0x0ff0000f)); ASSERT_TRUE(run(0x0000100f)); // FENCE and FENCE.I
    ASSERT_TRUE(run(immediate(0,1),4)); EXPECT_EQ(result(),5u);
    ASSERT_TRUE(run(immediate(0,2),4)); EXPECT_EQ(result(),6u); // same PC, different bytes
    ASSERT_TRUE(run(0x00100073)); EXPECT_TRUE(cpu->halted()); EXPECT_EQ(cpu->state().pc,4u);
}

TEST_F(RiscvMachine, ReservedUnsupportedAndCompressedFailWithoutCommit) {
    const std::uint32_t bad[]={0,0xffffffff,0x00000073,0x10500073,0x300022f3,
        regop(1,0x20),regop(1,1,0x3b),regop(4,0,0x3b),immediate(1,32,0x1b),
        immediate(5,0x800),immediate(7,0,0x03),immediate(1,0,0x67),0x0000200f};
    for (auto code:bad) {
        auto result=run(code,0x1234,0x5678);
        EXPECT_FALSE(result) << std::hex << code;
        EXPECT_NE(result.message.find("RISC-V at 0x0"),std::string::npos);
        EXPECT_EQ(cpu->state().pc,0u); EXPECT_EQ(this->result(),0xdeadbeefu);
        EXPECT_EQ(cpu->state().x[0],0u); EXPECT_FALSE(cpu->halted());
    }
}
