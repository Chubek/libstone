#pragma once
#include <cstdint>
#include <cstring>
#include <atomic>
#include "domvirt_types.hpp"
struct _object;

namespace domvirt::bindlet_bridge {

using PyObject = _object;

enum class RegisterClass : uint32_t {
    GPR = 1 << 0,
    SEGMENT = 1 << 1,
    CONTROL = 1 << 2,
    MSR = 1 << 3,
    FPU = 1 << 4,
    ALL = 0xFFFFFFFF,
};

constexpr RegisterClass operator|(RegisterClass lhs, RegisterClass rhs) noexcept {
    return static_cast<RegisterClass>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

class LazyRegisterProxy {
public:
    LazyRegisterProxy();
    ~LazyRegisterProxy();

    LazyRegisterProxy(const LazyRegisterProxy&) = delete;
    LazyRegisterProxy& operator=(const LazyRegisterProxy&) = delete;

    void update_from_kvm(int vcpu_fd);
    void commit_to_kvm(int vcpu_fd);

    bool is_dirty(RegisterClass cls) const;
    void mark_dirty(RegisterClass cls);
    void mark_clean(RegisterClass cls);

    const VcpuRegisters& regs() const { return regs_; }
    VcpuRegisters& regs() { mark_dirty(RegisterClass::GPR | RegisterClass::CONTROL); return regs_; }

    PyObject* to_python();
    void from_python(PyObject* obj);

private:
    VcpuRegisters regs_{};
#if defined(__x86_64__)
    struct kvm_sregs sregs_{};
#endif
    std::atomic<uint32_t> dirty_flags_{0};
    bool synced_{false};
};

inline bool LazyRegisterProxy::is_dirty(RegisterClass cls) const {
    return (dirty_flags_.load() & static_cast<uint32_t>(cls)) != 0;
}

inline void LazyRegisterProxy::mark_dirty(RegisterClass cls) {
    dirty_flags_.fetch_or(static_cast<uint32_t>(cls));
}

inline void LazyRegisterProxy::mark_clean(RegisterClass cls) {
    dirty_flags_.fetch_and(~static_cast<uint32_t>(cls));
}

} // namespace domvirt::bindlet_bridge
