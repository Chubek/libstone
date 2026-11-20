#pragma once
#include <string>
#include <vector>
#include <optional>
#include <linux/kvm.h>
#include "domvirt_types.hpp"

namespace domvirt::hypervisor {

struct KvmCapability {
    uint32_t cap;
    std::string name;
    bool supported;
};

class KvmDriver {
public:
    KvmDriver();
    ~KvmDriver();

    KvmDriver(const KvmDriver&) = delete;
    KvmDriver& operator=(const KvmDriver&) = delete;
    KvmDriver(KvmDriver&&) noexcept;
    KvmDriver& operator=(KvmDriver&&) noexcept;

    VmResult initialize();
    [[nodiscard]] int fd() const noexcept { return kvm_fd_; }
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] int api_version() const noexcept { return api_version_; }
    [[nodiscard]] int vcpu_mmap_size() const noexcept { return vcpu_mmap_size_; }

    bool check_extension(int cap) const;
    std::optional<int> get_extension(int cap) const;
    std::vector<KvmCapability> query_capabilities() const;

private:
    int kvm_fd_{-1};
    int api_version_{0};
    int vcpu_mmap_size_{0};
    bool initialized_{false};
};

} // namespace domvirt::hypervisor
