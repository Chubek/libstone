#include <gtest/gtest.h>
#include <cstring>
#include "bindlet_bridge/shared_memory.hpp"

using namespace domvirt::bindlet_bridge;

TEST(SharedMemoryViewTest, RegisterRegion) {
    SharedMemoryView view;
    uint8_t buf[4096] = {};
    view.register_region(0x1000, 4096, buf);
    auto span = view.get_span(0x1000, 128);
    EXPECT_EQ(span.size(), 128u);
}

TEST(SharedMemoryViewTest, MarkDirty) {
    SharedMemoryView view;
    uint8_t buf[4096] = {};
    view.register_region(0, 4096, buf);
    EXPECT_FALSE(view.is_dirty(0));
    view.mark_dirty(0, 128);
    EXPECT_TRUE(view.is_dirty(0));
}

TEST(SharedMemoryViewTest, OutOfBounds) {
    SharedMemoryView view;
    uint8_t buf[4096] = {};
    view.register_region(0, 4096, buf);
    auto span = view.get_span(0x999999, 128);
    EXPECT_TRUE(span.empty());
}
