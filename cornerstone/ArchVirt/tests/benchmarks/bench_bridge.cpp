#include <iostream>
#include <chrono>
#include "bindlet_bridge/bridge.hpp"

using namespace domvirt::bindlet_bridge;
using Clock = std::chrono::high_resolution_clock;

int main() {
    Bridge bridge;
    auto result = bridge.initialize();
    if (!result) {
        std::cerr << "Skipping: bridge init failed\n";
        return 0;
    }

    bool called = false;
    bridge.register_callback(BridgeEventType::VM_EXIT,
        [&](const BridgeEvent&) -> VmResult {
            called = true;
            return {VmErrorCode::SUCCESS, ""};
        });

    BridgeEvent event{};
    event.type = BridgeEventType::VM_EXIT;

    constexpr int ITERS = 100000;
    auto start = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        bridge.dispatch_event(event);
    }
    auto end = Clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    std::cout << "Bridge dispatch (" << ITERS << " iterations): "
              << elapsed.count() / ITERS << " ns/op\n";
    bridge.shutdown();
    return 0;
}
