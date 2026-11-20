#include "kvm_driver.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cerrno>

namespace domvirt::hypervisor {

KvmDriver::KvmDriver() = default;

KvmDriver::~KvmDriver() {
    if (kvm_fd_ >= 0) {
        close(kvm_fd_);
    }
}

KvmDriver::KvmDriver(KvmDriver&& other) noexcept
    : kvm_fd_(other.kvm_fd_)
    , api_version_(other.api_version_)
    , vcpu_mmap_size_(other.vcpu_mmap_size_)
    , initialized_(other.initialized_) {
    other.kvm_fd_ = -1;
    other.initialized_ = false;
}

KvmDriver& KvmDriver::operator=(KvmDriver&& other) noexcept {
    if (this != &other) {
        if (kvm_fd_ >= 0) close(kvm_fd_);
        kvm_fd_ = other.kvm_fd_;
        api_version_ = other.api_version_;
        vcpu_mmap_size_ = other.vcpu_mmap_size_;
        initialized_ = other.initialized_;
        other.kvm_fd_ = -1;
        other.initialized_ = false;
    }
    return *this;
}

VmResult KvmDriver::initialize() {
    kvm_fd_ = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd_ < 0) {
        return {VmErrorCode::KVM_OPEN_FAILED,
                "Failed to open /dev/kvm: " + std::string(strerror(errno))};
    }

    api_version_ = ioctl(kvm_fd_, KVM_GET_API_VERSION, 0);
    if (api_version_ != 12) {
        return {VmErrorCode::KVM_VERSION_MISMATCH,
                "KVM API version " + std::to_string(api_version_) + " != 12"};
    }

    vcpu_mmap_size_ = ioctl(kvm_fd_, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu_mmap_size_ < 0) {
        return {VmErrorCode::KVM_OPEN_FAILED,
                "Failed to get VCPU mmap size: " + std::string(strerror(errno))};
    }

    initialized_ = true;
    return {VmErrorCode::SUCCESS, "KVM driver initialized"};
}

bool KvmDriver::check_extension(int cap) const {
    if (kvm_fd_ < 0) return false;
    return ioctl(kvm_fd_, KVM_CHECK_EXTENSION, cap) > 0;
}

std::optional<int> KvmDriver::get_extension(int cap) const {
    if (kvm_fd_ < 0) return std::nullopt;
    int ret = ioctl(kvm_fd_, KVM_CHECK_EXTENSION, cap);
    if (ret <= 0) return std::nullopt;
    return ret;
}

std::vector<KvmCapability> KvmDriver::query_capabilities() const {
    std::vector<KvmCapability> caps;
    if (kvm_fd_ < 0) return caps;

    static constexpr std::pair<int, const char*> known[] = {
        {KVM_CAP_USER_MEMORY, "USER_MEMORY"},
        {KVM_CAP_SYNC_MMU, "SYNC_MMU"},
        {KVM_CAP_IRQCHIP, "IRQCHIP"},
        {KVM_CAP_DEVICE_CTRL, "DEVICE_CTRL"},
        {KVM_CAP_IOMMU, "IOMMU"},
        {KVM_CAP_TSC_DEADLINE_TIMER, "TSC_DEADLINE_TIMER"},
        {KVM_CAP_READONLY_MEM, "READONLY_MEM"},
        {KVM_CAP_MP_STATE, "MP_STATE"},
        {KVM_CAP_IRQ_ROUTING, "IRQ_ROUTING"},
        {KVM_CAP_MSI_DEVID, "MSI_DEVID"},
        {KVM_CAP_X86_ROBUST_SINGLESTEP, "X86_ROBUST_SINGLESTEP"},
        {KVM_CAP_SET_TSS_ADDR, "SET_TSS_ADDR"},
        {KVM_CAP_EXT_CPUID, "EXT_CPUID"},
        {KVM_CAP_VCPU_EVENTS, "VCPU_EVENTS"},
        {KVM_CAP_XSAVE, "XSAVE"},
        {KVM_CAP_IMMEDIATE_EXIT, "IMMEDIATE_EXIT"},
        {KVM_CAP_MAX_VCPUS, "MAX_VCPUS"},
        {KVM_CAP_MAX_VCPU_ID, "MAX_VCPU_ID"},
        {KVM_CAP_DIRTY_LOG_RING, "DIRTY_LOG_RING"},
    };

    for (auto& [cap, name] : known) {
        caps.push_back({static_cast<uint32_t>(cap), name, check_extension(cap)});
    }
    return caps;
}

} // namespace domvirt::hypervisor
