#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include "virtio/virtio_devices.hpp"
#include "virtio/virtqueue.hpp"

using namespace domvirt::virtio;
using Clock = std::chrono::high_resolution_clock;

int main() {
    constexpr int ITERS = 100000;
    std::vector<uint8_t> mem(65536);

    Virtqueue vq(256);
    vq.init(0, 4096, 8192);

    auto start = Clock::now();
    for (int i = 0; i < ITERS; ++i) {
        vq.avail_notify(mem.data());
    }
    auto end = Clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    std::cout << "Virtqueue avail_notify (" << ITERS << " iterations): "
              << elapsed.count() / ITERS << " ns/op\n";
    return 0;
}
