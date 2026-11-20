#pragma once
#include <cstdint>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <mutex>
#include <linux/kvm.h>
#include "kvm_driver.hpp"
#include "domvirt_types.hpp"

namespace domvirt {

namespace bindlet_bridge { class Bridge; }

namespace hypervisor {

class KvmVm;

using VmExitHandler = std::function<VmResult(struct kvm_run*)>;

struct VcpuConfig {
    bool reset_real_mode{false};
    uint64_t rip{0};
    uint64_t rsp{0};
    uint64_t cr0{0x60000010};
    uint64_t cr3{0};
    uint64_t cr4{0x20};
    uint64_t efer{0x500};
    int vcpu_id{0};
    uint64_t boot_argument{0};
};

class VcpuThread {
public:
    VcpuThread(KvmVm& vm, VcpuConfig config);
    ~VcpuThread();

    VcpuThread(const VcpuThread&) = delete;
    VcpuThread& operator=(const VcpuThread&) = delete;

    VmResult initialize();
    VmResult run();
    void stop();
    void join();
    VmResult result() const;

    VmResult get_regs(VcpuRegisters& regs);
    VmResult set_regs(const VcpuRegisters& regs);
    VmResult get_sregs(struct kvm_sregs& sregs);
    VmResult set_sregs(const struct kvm_sregs& sregs);

    [[nodiscard]] int fd() const noexcept { return vcpu_fd_; }
    [[nodiscard]] int id() const noexcept { return config_.vcpu_id; }
    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }
    [[nodiscard]] bool halted() const noexcept { return halted_.load(); }
    [[nodiscard]] struct kvm_run* run_struct() noexcept { return run_; }

    void set_exit_handler(VmExitHandler handler) { exit_handler_ = std::move(handler); }

private:
    void execution_loop();
    VmResult handle_exit();

    KvmVm& vm_;
    VcpuConfig config_;
    int vcpu_fd_{-1};
    int mmap_size_{0};
    struct kvm_run* run_{nullptr};
    std::unique_ptr<std::jthread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> halted_{false};
    std::atomic<bool> stop_requested_{false};
    VmExitHandler exit_handler_;
    mutable std::mutex result_mutex_;
    VmResult result_{};
};

} // namespace hypervisor
} // namespace domvirt
