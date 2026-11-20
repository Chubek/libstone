#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "virtio/virtio_devices.hpp"
#include "virtio/virtqueue.hpp"

using namespace domvirt::virtio;

TEST(VirtioNetTest, Init) {
    VirtioNet net;
    auto result = net.init();
    EXPECT_TRUE(result);
    EXPECT_EQ(net.device_id(), 1u);
}

TEST(VirtioBlkTest, Init) {
    VirtioBlk blk;
    auto result = blk.init();
    EXPECT_TRUE(result);
    EXPECT_EQ(blk.device_id(), 2u);
}

TEST(VirtioConsoleTest, Init) {
    VirtioConsole console;
    auto result = console.init();
    EXPECT_TRUE(result);
    EXPECT_EQ(console.device_id(), 3u);
}

TEST(VirtioDeviceTest, Reset) {
    VirtioNet net;
    net.init();
    net.reset();
    EXPECT_EQ(net.status(), 0u);
}

TEST(VirtioDeviceTest, MmioRead) {
    VirtioNet net;
    net.init();
    uint8_t buf[4] = {};
    EXPECT_TRUE(net.handle_mmio_read(0x00, buf, 4));
    uint32_t device_id = 0;
    memcpy(&device_id, buf, 4);
    EXPECT_EQ(device_id, 1u);
}
