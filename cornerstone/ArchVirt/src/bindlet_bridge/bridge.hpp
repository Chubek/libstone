#pragma once
#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>
#include "domvirt_types.hpp"
struct _object;

namespace domvirt::bindlet_bridge {

using ::domvirt::VmResult;
using ::domvirt::VmErrorCode;
using PyObject = _object;

class SharedMemoryView;
class LazyRegisterProxy;
class GuestBufferManager;

enum class BridgeEventType {
    VM_EXIT,
    MMIO_ACCESS,
    IO_ACCESS,
    HYPERCALL,
    BREAKPOINT,
    MEMORY_FAULT,
    DEVICE_READY,
    SHUTDOWN,
};

struct BridgeEvent {
    BridgeEventType type;
    uint64_t address{0};
    uint64_t value{0};
    uint32_t size{0};
    bool is_write{false};
    int vcpu_id{0};
    void* user_data{nullptr};
};

using BridgeCallback = std::function<VmResult(const BridgeEvent&)>;

class Bridge {
public:
    Bridge();
    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    VmResult initialize();
    void shutdown();

    VmResult execute_python(const std::string& module,
                            const std::string& function,
                            const std::string& json_args,
                            std::string& json_result);

    VmResult register_callback(BridgeEventType type, BridgeCallback cb);
    VmResult dispatch_event(const BridgeEvent& event);

    SharedMemoryView& shared_memory() { return *shared_mem_; }
    LazyRegisterProxy& lazy_regs() { return *lazy_regs_; }
    GuestBufferManager& buffer_manager() { return *buffer_mgr_; }

    void acquire_gil();
    void release_gil();
    [[nodiscard]] bool gil_held() const noexcept;

private:
    std::unique_ptr<SharedMemoryView> shared_mem_;
    std::unique_ptr<LazyRegisterProxy> lazy_regs_;
    std::unique_ptr<GuestBufferManager> buffer_mgr_;
    std::mutex callback_mutex_;
    std::vector<std::pair<BridgeEventType, BridgeCallback>> callbacks_;
    bool initialized_{false};
};

// Generated bindlet wrappers use this process-local bridge handle.
extern Bridge* g_bridge;

} // namespace domvirt::bindlet_bridge
