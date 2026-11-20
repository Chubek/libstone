#include <gtest/gtest.h>
#include "bindlet_bridge/bridge.hpp"
#include "bindlet_bridge/dispatcher.hpp"

using namespace domvirt::bindlet_bridge;

TEST(EventDispatcherTest, ClassifyFastPath) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    dispatcher.register_fast_path(KVM_EXIT_IO,
        [](const VmExitContext&) -> VmResult {
            return {VmErrorCode::SUCCESS, ""};
        });
    EXPECT_EQ(dispatcher.classify_exit(KVM_EXIT_IO), DispatchPath::FAST_PATH);
}

TEST(EventDispatcherTest, ClassifySlowPath) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    EXPECT_EQ(dispatcher.classify_exit(KVM_EXIT_UNKNOWN), DispatchPath::SLOW_PATH);
}

TEST(EventDispatcherTest, DispatchFastPath) {
    Bridge bridge;
    EventDispatcher dispatcher(bridge);
    bool called = false;
    dispatcher.register_fast_path(KVM_EXIT_IO,
        [&](const VmExitContext&) -> VmResult {
            called = true;
            return {VmErrorCode::SUCCESS, ""};
        });
    VmExitContext ctx{};
    ctx.exit_reason = KVM_EXIT_IO;
    dispatcher.dispatch(ctx);
    EXPECT_TRUE(called);
}
