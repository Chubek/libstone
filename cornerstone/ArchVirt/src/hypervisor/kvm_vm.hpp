#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <linux/kvm.h>
#include "kvm_driver.hpp"
#include "memory_manager.hpp"
#include "domvirt_types.hpp"

namespace domvirt {

namespace bindlet_bridge { class Bridge; }

namespace hypervisor {

class VcpuThread;

class KvmVm {
public:
    KvmVm(KvmDriver& driver);
    ~KvmVm();

    KvmVm(const KvmVm&) = delete;
    KvmVm& operator=(const KvmVm&) = delete;

    VmResult create();
    VmResult create_irq_chip();
    VmResult create_vcpu(int vcpu_id);
    VmResult set_tss_addr(uint64_t addr);
    VmResult set_identity_map_addr(uint64_t addr);

    [[nodiscard]] int fd() const noexcept { return vm_fd_; }
    [[nodiscard]] MemoryManager& memory() noexcept { return memory_; }
    [[nodiscard]] const MemoryManager& memory() const noexcept { return memory_; }
    [[nodiscard]] KvmDriver& driver() noexcept { return driver_; }

    void set_bridge(bindlet_bridge::Bridge* bridge) { bridge_ = bridge; }
    bindlet_bridge::Bridge* bridge() const noexcept { return bridge_; }

private:
    KvmDriver& driver_;
    int vm_fd_{-1};
    MemoryManager memory_;
    bindlet_bridge::Bridge* bridge_{nullptr};
    struct kvm_pit_config pit_config_{};
};

} // namespace hypervisor
} // namespace domvirt
