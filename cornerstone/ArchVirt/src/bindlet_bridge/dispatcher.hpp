#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include "bridge.hpp"
#include "domvirt_types.hpp"
#include "file_transfer.hpp"

namespace domvirt::bindlet_bridge {

enum class DispatchPath {
    FAST_PATH,
    SLOW_PATH,
};

struct VmExitContext {
    int vcpu_id;
    uint32_t exit_reason;
    uint64_t exit_qualification;
    uint64_t guest_phys_addr;
    uint64_t guest_rip;
    bool is_write;
    uint32_t len;
    uint8_t* data;
};

class EventDispatcher {
public:
    EventDispatcher(Bridge& bridge);
    ~EventDispatcher() = default;

    void register_fast_path(uint32_t exit_reason, std::function<VmResult(const VmExitContext&)> handler);
    void register_slow_path(uint32_t exit_reason, std::function<VmResult(const VmExitContext&)> handler);

    DispatchPath classify_exit(uint32_t exit_reason) const;
    VmResult dispatch(const VmExitContext& ctx);

    void set_fast_path_filter(uint64_t filter_bitmap);
    [[nodiscard]] bool is_fast_path(uint32_t exit_reason) const;

    void register_file_transfer_engine(FileTransferEngine* engine);
    FileTransferEngine* file_transfer_engine() const { return ft_engine_; }

private:
    Bridge& bridge_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<uint32_t, std::function<VmResult(const VmExitContext&)>> fast_path_handlers_;
    std::unordered_map<uint32_t, std::function<VmResult(const VmExitContext&)>> slow_path_handlers_;
    uint64_t fast_path_filter_{0};
    FileTransferEngine* ft_engine_{nullptr};
};

} // namespace domvirt::bindlet_bridge
