#pragma once
#include <cstdint>
#include <linux/kvm.h>
#include "domvirt_types.hpp"

namespace domvirt::interrupts {

class IrqChip {
public:
    IrqChip() = default;
    ~IrqChip() = default;

    VmResult create(int vm_fd);
    VmResult set_irq_line(int vm_fd, uint32_t irq, int level);
    VmResult create_irqfd(int vm_fd, uint32_t gsi, int eventfd);

    VmResult inject_nmi(int vcpu_fd);
    VmResult set_irq_level(int vm_fd, uint32_t irq, bool level);

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

private:
    bool initialized_{false};
};

class MsiRoutingTable {
public:
    MsiRoutingTable() = default;
    ~MsiRoutingTable();

    VmResult initialize(int vm_fd);
    VmResult add_msi_route(uint32_t gsi, uint32_t dest_vcpu,
                           uint32_t vector, uint32_t flags);
    VmResult commit(int vm_fd);
    VmResult signal_msi(int vm_fd, uint32_t gsi);

    [[nodiscard]] uint32_t num_entries() const noexcept { return num_entries_; }

private:
    struct kvm_irq_routing* routing_{nullptr};
    uint32_t num_entries_{0};
    uint32_t max_entries_{0};
    bool initialized_{false};
};

} // namespace domvirt::interrupts
