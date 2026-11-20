#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bindlet_bridge/bridge.hpp"
#include "bindlet_bridge/dispatcher.hpp"

using namespace domvirt::bindlet_bridge;

TEST(BridgeIpcTest, BridgeCanBeConstructed) {
    Bridge bridge;
    SUCCEED();
}

TEST(BridgeIpcTest, RegisterCallbackSucceeds) {
    Bridge bridge;
    auto result = bridge.register_callback(BridgeEventType::VM_EXIT,
        [](const BridgeEvent&) { return VmResult{VmErrorCode::SUCCESS, ""}; });
    EXPECT_TRUE(result);
}

TEST(BridgeIpcTest, DispatchWithoutCallbackSucceeds) {
    Bridge bridge;
    BridgeEvent event{};
    event.type = BridgeEventType::IO_ACCESS;
    EXPECT_TRUE(bridge.dispatch_event(event));
}

TEST(BridgeIpcTest, DispatchInvokesMatchingCallback) {
    Bridge bridge;
    bool called = false;
    bridge.register_callback(BridgeEventType::VM_EXIT,
        [&](const BridgeEvent&) {
            called = true;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::VM_EXIT;
    EXPECT_TRUE(bridge.dispatch_event(event));
    EXPECT_TRUE(called);
}

TEST(BridgeIpcTest, DispatchPassesAddress) {
    Bridge bridge;
    std::uint64_t seen = 0;
    bridge.register_callback(BridgeEventType::MMIO_ACCESS,
        [&](const BridgeEvent& ev) {
            seen = ev.address;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::MMIO_ACCESS;
    event.address = 0x1234;
    bridge.dispatch_event(event);
    EXPECT_EQ(seen, 0x1234u);
}

TEST(BridgeIpcTest, DispatchPassesValue) {
    Bridge bridge;
    std::uint64_t seen = 0;
    bridge.register_callback(BridgeEventType::MMIO_ACCESS,
        [&](const BridgeEvent& ev) {
            seen = ev.value;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::MMIO_ACCESS;
    event.value = 0xfeedface;
    bridge.dispatch_event(event);
    EXPECT_EQ(seen, 0xfeedfaceu);
}

TEST(BridgeIpcTest, DispatchPassesSize) {
    Bridge bridge;
    std::uint32_t seen = 0;
    bridge.register_callback(BridgeEventType::IO_ACCESS,
        [&](const BridgeEvent& ev) {
            seen = ev.size;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::IO_ACCESS;
    event.size = 8;
    bridge.dispatch_event(event);
    EXPECT_EQ(seen, 8u);
}

TEST(BridgeIpcTest, DispatchPassesWriteFlag) {
    Bridge bridge;
    bool seen = false;
    bridge.register_callback(BridgeEventType::IO_ACCESS,
        [&](const BridgeEvent& ev) {
            seen = ev.is_write;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::IO_ACCESS;
    event.is_write = true;
    bridge.dispatch_event(event);
    EXPECT_TRUE(seen);
}

TEST(BridgeIpcTest, DispatchPassesVcpuId) {
    Bridge bridge;
    int seen = -1;
    bridge.register_callback(BridgeEventType::HYPERCALL,
        [&](const BridgeEvent& ev) {
            seen = ev.vcpu_id;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::HYPERCALL;
    event.vcpu_id = 7;
    bridge.dispatch_event(event);
    EXPECT_EQ(seen, 7);
}

TEST(BridgeIpcTest, FirstMatchingCallbackWins) {
    Bridge bridge;
    int seen = 0;
    bridge.register_callback(BridgeEventType::BREAKPOINT,
        [&](const BridgeEvent&) {
            seen = 1;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    bridge.register_callback(BridgeEventType::BREAKPOINT,
        [&](const BridgeEvent&) {
            seen = 2;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::BREAKPOINT;
    bridge.dispatch_event(event);
    EXPECT_EQ(seen, 1);
}

TEST(BridgeIpcTest, CallbackResultPropagates) {
    Bridge bridge;
    bridge.register_callback(BridgeEventType::SHUTDOWN,
        [&](const BridgeEvent&) {
            return VmResult{VmErrorCode::BRIDGE_ERROR, "boom"};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::SHUTDOWN;
    auto result = bridge.dispatch_event(event);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.code, VmErrorCode::BRIDGE_ERROR);
}

TEST(BridgeIpcTest, DifferentEventTypesDoNotCollide) {
    Bridge bridge;
    bool io_called = false;
    bool mmio_called = false;
    bridge.register_callback(BridgeEventType::IO_ACCESS,
        [&](const BridgeEvent&) {
            io_called = true;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    bridge.register_callback(BridgeEventType::MMIO_ACCESS,
        [&](const BridgeEvent&) {
            mmio_called = true;
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    BridgeEvent event{};
    event.type = BridgeEventType::MMIO_ACCESS;
    bridge.dispatch_event(event);
    EXPECT_FALSE(io_called);
    EXPECT_TRUE(mmio_called);
}

TEST(BridgeIpcTest, DispatcherClassifiesFastPath) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    dispatcher.register_fast_path(1, [](const VmExitContext&) {
        return VmResult{VmErrorCode::SUCCESS, ""};
    });
    EXPECT_EQ(dispatcher.classify_exit(1), DispatchPath::FAST_PATH);
}

TEST(BridgeIpcTest, DispatcherFallsBackToSlowPath) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    EXPECT_EQ(dispatcher.classify_exit(2), DispatchPath::SLOW_PATH);
}

TEST(BridgeIpcTest, FastPathHandlerRuns) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    bool called = false;
    dispatcher.register_fast_path(3, [&](const VmExitContext& ctx) {
        called = true;
        EXPECT_EQ(ctx.guest_phys_addr, 0x4444u);
        return VmResult{VmErrorCode::SUCCESS, ""};
    });
    VmExitContext ctx{};
    ctx.exit_reason = 3;
    ctx.guest_phys_addr = 0x4444;
    EXPECT_TRUE(dispatcher.dispatch(ctx));
    EXPECT_TRUE(called);
}

TEST(BridgeIpcTest, SlowPathHandlerRuns) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    bool called = false;
    dispatcher.register_slow_path(4, [&](const VmExitContext& ctx) {
        called = true;
        EXPECT_EQ(ctx.exit_reason, 4u);
        return VmResult{VmErrorCode::SUCCESS, ""};
    });
    VmExitContext ctx{};
    ctx.exit_reason = 4;
    EXPECT_TRUE(dispatcher.dispatch(ctx));
    EXPECT_TRUE(called);
}

TEST(BridgeIpcTest, DispatcherFallsBackToBridgeEvent) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    bool called = false;
    bridge.register_callback(BridgeEventType::VM_EXIT,
        [&](const BridgeEvent& ev) {
            called = true;
            EXPECT_EQ(ev.address, 0x5555u);
            return VmResult{VmErrorCode::SUCCESS, ""};
        });
    VmExitContext ctx{};
    ctx.exit_reason = 5;
    ctx.guest_phys_addr = 0x5555;
    EXPECT_TRUE(dispatcher.dispatch(ctx));
    EXPECT_TRUE(called);
}

TEST(BridgeIpcTest, FastPathFilterBitSet) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    dispatcher.set_fast_path_filter(1ull << 6);
    EXPECT_TRUE(dispatcher.is_fast_path(6));
}

TEST(BridgeIpcTest, FastPathFilterBitUnset) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    dispatcher.set_fast_path_filter(1ull << 6);
    EXPECT_FALSE(dispatcher.is_fast_path(5));
}

TEST(BridgeIpcTest, FastPathFilterIgnoresLargeExitReasons) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    dispatcher.set_fast_path_filter(~0ull);
    EXPECT_FALSE(dispatcher.is_fast_path(64));
}
