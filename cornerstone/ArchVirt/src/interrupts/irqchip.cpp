#include "irqchip.hpp"
#include <sys/ioctl.h>
#include <cstring>
#include <cerrno>

namespace domvirt::interrupts {

VmResult IrqChip::create(int vm_fd) {
    if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "KVM_CREATE_IRQCHIP failed: " + std::string(strerror(errno))};
    }

    struct kvm_pit_config pit_config{};
    pit_config.flags = 0;
    if (ioctl(vm_fd, KVM_CREATE_PIT2, &pit_config) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "KVM_CREATE_PIT2 failed: " + std::string(strerror(errno))};
    }

    initialized_ = true;
    return {VmErrorCode::SUCCESS, "IRQ chip created"};
}

VmResult IrqChip::set_irq_line(int vm_fd, uint32_t irq, int level) {
    struct kvm_irq_level irq_level{};
    irq_level.irq = irq;
    irq_level.level = level;

    if (ioctl(vm_fd, KVM_IRQ_LINE, &irq_level) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "KVM_IRQ_LINE failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, ""};
}

VmResult IrqChip::create_irqfd(int vm_fd, uint32_t gsi, int eventfd) {
    struct kvm_irqfd irqfd_data{};
    irqfd_data.fd = eventfd;
    irqfd_data.gsi = gsi;
    irqfd_data.flags = 0;

    if (ioctl(vm_fd, KVM_IRQFD, &irqfd_data) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "KVM_IRQFD failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, ""};
}

VmResult IrqChip::inject_nmi(int vcpu_fd) {
    if (ioctl(vcpu_fd, KVM_NMI, 0) < 0) {
        return {VmErrorCode::INTERRUPT_ERROR,
                "KVM_NMI failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, "NMI injected"};
}

VmResult IrqChip::set_irq_level(int vm_fd, uint32_t irq, bool level) {
    return set_irq_line(vm_fd, irq, level ? 1 : 0);
}

} // namespace domvirt::interrupts
