#include "dispatcher.hpp"
#include <algorithm>
#include "file_transfer.hpp"

namespace domvirt::bindlet_bridge {

EventDispatcher::EventDispatcher(Bridge& bridge)
    : bridge_(bridge) {}

void EventDispatcher::register_fast_path(uint32_t exit_reason,
                                         std::function<VmResult(const VmExitContext&)> handler) {
    std::unique_lock lock(mutex_);
    fast_path_handlers_[exit_reason] = std::move(handler);
}

void EventDispatcher::register_slow_path(uint32_t exit_reason,
                                         std::function<VmResult(const VmExitContext&)> handler) {
    std::unique_lock lock(mutex_);
    slow_path_handlers_[exit_reason] = std::move(handler);
}

DispatchPath EventDispatcher::classify_exit(uint32_t exit_reason) const {
    std::shared_lock lock(mutex_);
    if (fast_path_handlers_.count(exit_reason)) {
        return DispatchPath::FAST_PATH;
    }
    return DispatchPath::SLOW_PATH;
}

VmResult EventDispatcher::dispatch(const VmExitContext& ctx) {
    std::shared_lock lock(mutex_);

    auto fast_it = fast_path_handlers_.find(ctx.exit_reason);
    if (fast_it != fast_path_handlers_.end()) {
        return fast_it->second(ctx);
    }

    auto slow_it = slow_path_handlers_.find(ctx.exit_reason);
    if (slow_it != slow_path_handlers_.end()) {
        return slow_it->second(ctx);
    }

    BridgeEvent event{};
    event.type = BridgeEventType::VM_EXIT;
    event.address = ctx.guest_phys_addr;
    event.is_write = ctx.is_write;
    event.size = ctx.len;
    event.vcpu_id = ctx.vcpu_id;

    return bridge_.dispatch_event(event);
}

void EventDispatcher::set_fast_path_filter(uint64_t filter_bitmap) {
    std::unique_lock lock(mutex_);
    fast_path_filter_ = filter_bitmap;
}

bool EventDispatcher::is_fast_path(uint32_t exit_reason) const {
    std::shared_lock lock(mutex_);
    if (exit_reason >= 64) return false;
    return (fast_path_filter_ & (1ULL << exit_reason)) != 0;
}

void EventDispatcher::register_file_transfer_engine(FileTransferEngine* engine) {
    std::unique_lock lock(mutex_);
    ft_engine_ = engine;
}

} // namespace domvirt::bindlet_bridge
