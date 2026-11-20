#include "kvm_vm.hpp"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace domvirt::hypervisor {

KvmVm::KvmVm(KvmDriver& driver)
    : driver_(driver) {}

KvmVm::~KvmVm() {
    if (vm_fd_ >= 0) close(vm_fd_);
}

VmResult KvmVm::create() {
    if (!driver_.is_initialized()) {
        return {VmErrorCode::VM_CREATE_FAILED, "KVM driver not initialized"};
    }

    vm_fd_ = ioctl(driver_.fd(), KVM_CREATE_VM, 0);
    if (vm_fd_ < 0) {
        return {VmErrorCode::VM_CREATE_FAILED,
                "KVM_CREATE_VM failed: " + std::string(strerror(errno))};
    }

    return {VmErrorCode::SUCCESS, "VM created"};
}

VmResult KvmVm::create_irq_chip() {
    if (vm_fd_ < 0) {
        return {VmErrorCode::VM_CREATE_FAILED, "VM not created"};
    }

    if (ioctl(vm_fd_, KVM_CREATE_IRQCHIP, 0) < 0) {
        return {VmErrorCode::DEVICE_INIT_FAILED,
                "KVM_CREATE_IRQCHIP failed: " + std::string(strerror(errno))};
    }

    if (ioctl(vm_fd_, KVM_CREATE_PIT2, &pit_config_) < 0) {
        return {VmErrorCode::DEVICE_INIT_FAILED,
                "KVM_CREATE_PIT2 failed: " + std::string(strerror(errno))};
    }

    return {VmErrorCode::SUCCESS, "IRQ chip created"};
}

VmResult KvmVm::create_vcpu(int vcpu_id) {
    if (vm_fd_ < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED, "VM not created"};
    }

    int vcpu_fd = ioctl(vm_fd_, KVM_CREATE_VCPU, vcpu_id);
    if (vcpu_fd < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_CREATE_VCPU failed: " + std::string(strerror(errno))};
    }

    close(vcpu_fd);
    return {VmErrorCode::SUCCESS, "VCPU " + std::to_string(vcpu_id) + " created"};
}

VmResult KvmVm::set_tss_addr(uint64_t addr) {
    if (vm_fd_ < 0) return {VmErrorCode::VM_CREATE_FAILED, "VM not created"};
    if (ioctl(vm_fd_, KVM_SET_TSS_ADDR, addr) < 0) {
        return {VmErrorCode::DEVICE_INIT_FAILED,
                "KVM_SET_TSS_ADDR failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, "TSS address set"};
}

VmResult KvmVm::set_identity_map_addr(uint64_t addr) {
    if (vm_fd_ < 0) return {VmErrorCode::VM_CREATE_FAILED, "VM not created"};
    if (ioctl(vm_fd_, KVM_SET_IDENTITY_MAP_ADDR, &addr) < 0) {
        return {VmErrorCode::DEVICE_INIT_FAILED,
                "KVM_SET_IDENTITY_MAP_ADDR failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, "Identity map address set"};
}

} // namespace domvirt::hypervisor
