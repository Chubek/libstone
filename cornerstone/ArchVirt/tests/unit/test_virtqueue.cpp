#include <gtest/gtest.h>
#include <cstring>
#include "virtio/virtqueue.hpp"

using namespace domvirt::virtio;

TEST(VirtqueueTest, Init) {
    Virtqueue vq(256);
    EXPECT_TRUE(vq.init(0, 0, 0));
    EXPECT_TRUE(vq.is_ready());
}

TEST(VirtqueueTest, Reset) {
    Virtqueue vq(256);
    vq.init(0, 0, 0);
    vq.reset();
    EXPECT_FALSE(vq.is_ready());
}

TEST(VirtqueueTest, AvailNotifyNoMem) {
    Virtqueue vq(256);
    vq.init(0, 0, 0);
    EXPECT_FALSE(vq.avail_notify(nullptr));
}
