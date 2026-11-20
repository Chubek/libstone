#pragma once
#include <array>
#include <functional>
#include <unordered_set>
#include "machine/profile.hpp"
#include "hypervisor/memory_manager.hpp"

namespace domvirt::virtualization {

struct Aarch64State {
    std::array<std::uint64_t, 31> x{};
    std::uint64_t sp{}, pc{};
    std::uint32_t nzcv{};
};

class Aarch64Cpu {
public:
    using Serial = std::function<std::uint8_t(bool, std::uint8_t)>;
    Aarch64Cpu(hypervisor::MemoryManager& memory, const machine::Profile& profile,
               std::uint64_t entry, std::uint64_t argument, Serial serial);
    VmResult step();
    bool halted() const { return halted_; }
    const Aarch64State& state() const { return state_; }
    void set_state(const Aarch64State& state) { state_ = state; }
private:
    bool condition(unsigned code) const;
    std::uint64_t reg(unsigned index, bool sp = false) const;
    void write(unsigned index, std::uint64_t value, unsigned width, bool sp = false);
    std::uint64_t load(std::uint64_t address, unsigned bytes);
    void store(std::uint64_t address, unsigned bytes, std::uint64_t value);
    void flags(std::uint64_t a, std::uint64_t b, std::uint64_t result, unsigned width, bool sub);
    void execute(std::uint32_t word);
    hypervisor::MemoryManager& memory_;
    const machine::Profile& profile_;
    Aarch64State state_{};
    Serial serial_;
    bool halted_{false};
    std::unordered_set<std::uint32_t> decoded_;
};

} // namespace domvirt::virtualization
