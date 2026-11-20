#include <gtest/gtest.h>
#include "bindlet_bridge/bridge.hpp"

using namespace domvirt::bindlet_bridge;

TEST(BridgeTest, Initialize) {
    Bridge bridge;
    auto result = bridge.initialize();
    EXPECT_TRUE(result);
    bridge.shutdown();
}

TEST(BridgeTest, DoubleInit) {
    Bridge bridge;
    bridge.initialize();
    auto result = bridge.initialize();
    EXPECT_TRUE(result);
    bridge.shutdown();
}

TEST(BridgeTest, RegisterCallback) {
    Bridge bridge;
    bridge.initialize();
    bool called = false;
    auto result = bridge.register_callback(BridgeEventType::VM_EXIT,
        [&](const BridgeEvent&) -> VmResult {
            called = true;
            return {VmErrorCode::SUCCESS, ""};
        });
    EXPECT_TRUE(result);

    BridgeEvent event{};
    event.type = BridgeEventType::VM_EXIT;
    bridge.dispatch_event(event);
    EXPECT_TRUE(called);
    bridge.shutdown();
}

TEST(BridgeTest, DispatchNoCallback) {
    Bridge bridge;
    BridgeEvent event{};
    event.type = BridgeEventType::VM_EXIT;
    auto result = bridge.dispatch_event(event);
    EXPECT_TRUE(result);
}
