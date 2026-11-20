#include "irqchip.hpp"
#include <sys/ioctl.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>

namespace domvirt::interrupts {

MsiRoutingTable::~MsiRoutingTable() {
    if (routing_) {
        free(routing_);
    }
}

VmResult MsiRoutingTable::initialize(int vm_fd) {
    max_entries_ = 256;
    size_t routing_size = sizeof(struct kvm_irq_routing) +
                          max_entries_ * sizeof(struct kvm_irq_routing_entry);
    routing_ = static_cast<struct kvm_irq_routing*>(calloc(1, routing_size));
    if (!routing_) {
        return {VmErrorCode::INTERRUPT_ERROR, "Failed to allocate routing table"};
    }
    routing_->nr = 0;
    routing_->flags = 0;
    initialized_ = true;
    return {VmErrorCode::SUCCESS, "MSI routing table initialized"};
}

VmResult MsiRoutingTable::add_msi_route(uint32_t gsi, uint32_t dest_vcpu,
                                        uint32_t vector, uint32_t flags) {
    if (!initialized_ || num_entries_ >= max_entries_) {
        return {VmErrorCode::INTERRUPT_ERROR, "Routing table full"};
    }

    auto& entry = routing_->entries[num_entries_];
    entry.gsi = gsi;
    entry.type = KVM_IRQ_ROUTING_MSI;
    entry.flags = flags;
    entry.u.msi.address_lo = 0xFEE00000 | (dest_vcpu << 12);
    entry.u.msi.address_hi = 0;
    entry.u.msi.data = vector;

    num_entries_++;
    routing_->nr = num_entries_;
    return {VmErrorCode::SUCCESS, "MSI route added"};
}

VmResult MsiRoutingTable::commit(int vm_fd) {
    if (!initialized_) {
        return {VmErrorCode::INTERRUPT_ERROR, "Routing table not initialized"};
    }

    if (ioctl(vm_fd, KVM_SET_GSI_ROUTING, routing_) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "KVM_SET_GSI_ROUTING failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, "MSI routing committed"};
}

VmResult MsiRoutingTable::signal_msi(int vm_fd, uint32_t gsi) {
    struct kvm_irq_level irq_level{};
    irq_level.irq = gsi;
    irq_level.level = 1;

    if (ioctl(vm_fd, KVM_IRQ_LINE, &irq_level) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "MSI signal failed: " + std::string(strerror(errno))};
    }

    irq_level.level = 0;
    ioctl(vm_fd, KVM_IRQ_LINE, &irq_level);

    return {VmErrorCode::SUCCESS, "MSI signaled"};
}

} // namespace domvirt::interrupts
