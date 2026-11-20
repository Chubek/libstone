#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "domvirt_types.hpp"
#include "hypervisor/memory_manager.hpp"

namespace domvirt::bindlet_bridge {
class Bridge;
}

namespace domvirt::virtualization {

struct VirtualCpuConfig {
    std::uint64_t rip{0};
    std::uint64_t rsp{0};
    std::uint64_t cr0{0x60000010};
    std::uint64_t cr3{0};
    std::uint64_t cr4{0x20};
    std::uint64_t efer{0x500};
    int vcpu_id{0};
    std::uint64_t boot_argument{0};
};

using VirtualIoHandler = std::function<std::uint8_t(std::uint16_t port,
                                                     bool is_write,
                                                     std::uint8_t size,
                                                     std::uint8_t value)>;

/**
 * Small software x86-64 execution engine.
 *
 * This is deliberately separate from the KVM VCPU implementation. It uses
 * Zydis to decode guest instructions and interprets the integer/serial
 * instruction subset needed by the freestanding Domix guest. Unsupported
 * instructions fail explicitly instead of executing host code.
 */
class VirtualCpu {
public:
    VirtualCpu(hypervisor::MemoryManager& memory, VirtualCpuConfig config);
    ~VirtualCpu();

    VirtualCpu(const VirtualCpu&) = delete;
    VirtualCpu& operator=(const VirtualCpu&) = delete;

    VmResult initialize();
    VmResult run();
    void stop();
    void join();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }
    [[nodiscard]] bool halted() const noexcept { return halted_.load(); }
    [[nodiscard]] std::uint64_t instruction_count() const noexcept {
        return instruction_count_.load();
    }
    [[nodiscard]] VmResult result() const;
    [[nodiscard]] VcpuRegisters registers() const;

    void set_io_handler(VirtualIoHandler handler) { io_handler_ = std::move(handler); }
    void set_bridge(bindlet_bridge::Bridge* bridge) { bridge_ = bridge; }

    // Synchronous single-step entry point used by tests and diagnostics.
    VmResult step();

private:
    struct RegisterRef {
        unsigned index{0};
        unsigned width{0};
        bool high8{false};
        bool valid{false};
    };

    void execution_loop();
    VmResult execute_instruction();
    RegisterRef register_ref(std::uint32_t reg) const;
    std::uint64_t read_register(const RegisterRef& reg) const;
    void write_register(const RegisterRef& reg, std::uint64_t value);
    bool read_memory(std::uint64_t address, unsigned width, std::uint64_t& value) const;
    bool write_memory(std::uint64_t address, unsigned width, std::uint64_t value);
    bool operand_address(const void* operand, std::uint64_t instruction_address,
                         std::uint8_t instruction_length, std::uint64_t& address) const;
    bool read_operand(const void* operand, std::uint64_t instruction_address,
                      std::uint8_t instruction_length, std::uint64_t& value) const;
    bool write_operand(const void* operand, std::uint64_t instruction_address,
                       std::uint8_t instruction_length, std::uint64_t value);
    std::uint64_t branch_target(const void* operand, std::uint64_t next_rip) const;
    void set_logic_flags(std::uint64_t value, unsigned width);
    void set_add_flags(std::uint64_t lhs, std::uint64_t rhs,
                       std::uint64_t result, unsigned width);
    void set_sub_flags(std::uint64_t lhs, std::uint64_t rhs,
                       std::uint64_t result, unsigned width);
    bool flag(std::uint64_t mask) const noexcept { return (rflags_ & mask) != 0; }
    void set_flag(std::uint64_t mask, bool value) noexcept;
    VmResult failure(VmErrorCode code, std::string message);

    hypervisor::MemoryManager& memory_;
    VirtualCpuConfig config_;
    std::uint64_t gpr_[16]{};
    std::uint64_t rip_{0};
    std::uint64_t rflags_{0x2};
    std::uint64_t cr0_{0};
    std::uint64_t cr3_{0};
    std::uint64_t cr4_{0};
    std::uint64_t efer_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> halted_{false};
    std::atomic<std::uint64_t> instruction_count_{0};
    mutable std::mutex result_mutex_;
    VmResult result_{};
    std::unique_ptr<std::jthread> thread_;
    VirtualIoHandler io_handler_;
    bindlet_bridge::Bridge* bridge_{nullptr};
    void* decoder_storage_{nullptr};
};

} // namespace domvirt::virtualization
