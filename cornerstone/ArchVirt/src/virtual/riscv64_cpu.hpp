#pragma once
#include <array>
#include <functional>
#include <unordered_set>
#include "machine/profile.hpp"
#include "hypervisor/memory_manager.hpp"

namespace domvirt::virtualization {

struct Riscv64State {
    std::array<std::uint64_t, 32> x{};
    std::uint64_t pc{};
};

// Flat, single-hart RV64IM + Zifencei execution environment. EBREAK is the
// DomIOS terminal halt operation; faults fail the VM rather than enter a CSR trap.
class Riscv64Cpu {
public:
    using Serial = std::function<std::uint8_t(bool, std::uint8_t)>;
    Riscv64Cpu(hypervisor::MemoryManager& memory, const machine::Profile& profile,
               std::uint64_t entry, std::uint64_t argument, Serial serial);
    VmResult step();
    bool halted() const { return halted_; }
    const Riscv64State& state() const { return state_; }
    void set_state(const Riscv64State& state) { state_ = state; state_.x[0] = 0; }
private:
    std::uint64_t load(std::uint64_t address, unsigned bytes);
    void store(std::uint64_t address, unsigned bytes, std::uint64_t value);
    void execute(std::uint32_t instruction);
    hypervisor::MemoryManager& memory_;
    const machine::Profile& profile_;
    Riscv64State state_{};
    Serial serial_;
    bool halted_{false};
    std::unordered_set<std::uint32_t> decoded_;
};

} // namespace domvirt::virtualization
