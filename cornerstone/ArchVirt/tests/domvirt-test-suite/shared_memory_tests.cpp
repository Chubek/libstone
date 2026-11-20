#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "bindlet_bridge/shared_memory.hpp"
#include "test_helpers.hpp"

using namespace domvirt::bindlet_bridge;
using namespace domvirt::test_support;

TEST(SharedMemoryTest, RegisterRegionAndSpan) {
    SharedMemoryView view;
    std::array<std::uint8_t, 8> backing{1, 2, 3, 4, 5, 6, 7, 8};
    view.register_region(0x1000, backing.size(), backing.data());
    auto span = view.get_span(0x1000, 4);
    ASSERT_EQ(span.size(), 4u);
    EXPECT_EQ(span[0], 1);
    EXPECT_EQ(span[3], 4);
}

TEST(SharedMemoryTest, ConstSpanMatchesMutableSpan) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{9, 8, 7, 6};
    view.register_region(0x2000, backing.size(), backing.data());
    const auto& cref = view;
    auto span = cref.get_span(0x2000, 4);
    ASSERT_EQ(span.size(), 4u);
    EXPECT_EQ(span[1], 8);
}

TEST(SharedMemoryTest, OutOfRangeSpanIsEmpty) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{1, 2, 3, 4};
    view.register_region(0x3000, backing.size(), backing.data());
    auto span = view.get_span(0x4000, 1);
    EXPECT_TRUE(span.empty());
}

TEST(SharedMemoryTest, DirtyFlagStartsCleared) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{1, 2, 3, 4};
    view.register_region(0x5000, backing.size(), backing.data());
    EXPECT_FALSE(view.is_dirty(0x5000));
}

TEST(SharedMemoryTest, MarkDirtySetsFlag) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{1, 2, 3, 4};
    view.register_region(0x6000, backing.size(), backing.data());
    view.mark_dirty(0x6000, 1);
    EXPECT_TRUE(view.is_dirty(0x6000));
}

TEST(SharedMemoryTest, SyncDirtyClearsFlag) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{1, 2, 3, 4};
    view.register_region(0x7000, backing.size(), backing.data());
    view.mark_dirty(0x7000, 4);
    view.sync_dirty();
    EXPECT_FALSE(view.is_dirty(0x7000));
}

TEST(SharedMemoryTest, UnregisterAllClearsRegions) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{1, 2, 3, 4};
    view.register_region(0x8000, backing.size(), backing.data());
    view.unregister_all();
    EXPECT_TRUE(view.get_span(0x8000, 1).empty());
}

TEST(SharedMemoryTest, MultipleRegionsStayIsolated) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> first{1, 2, 3, 4};
    std::array<std::uint8_t, 4> second{5, 6, 7, 8};
    view.register_region(0x9000, first.size(), first.data());
    view.register_region(0xA000, second.size(), second.data());
    auto a = view.get_span(0x9000, 4);
    auto b = view.get_span(0xA000, 4);
    EXPECT_EQ(a[0], 1);
    EXPECT_EQ(b[0], 5);
}

TEST(SharedMemoryTest, SpanWritesReachBackingStore) {
    SharedMemoryView view;
    std::array<std::uint8_t, 4> backing{1, 2, 3, 4};
    view.register_region(0xB000, backing.size(), backing.data());
    auto span = view.get_span(0xB000, 4);
    span[2] = 42;
    EXPECT_EQ(backing[2], 42);
}

TEST(SharedMemoryTest, GuestBufferAllocatesAlignedStorage) {
    GuestBufferManager mgr;
    auto* buf = mgr.allocate_guest_buffer(13, 0x1000);
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf) % domvirt::PAGE_SIZE, 0u);
}

TEST(SharedMemoryTest, GuestBufferZeroAllocationFails) {
    GuestBufferManager mgr;
    EXPECT_EQ(mgr.allocate_guest_buffer(0, 0x1000), nullptr);
}

TEST(SharedMemoryTest, GuestBufferValidateAccessWorks) {
    GuestBufferManager mgr;
    auto* buf = mgr.allocate_guest_buffer(64, 0x2000);
    ASSERT_NE(buf, nullptr);
    EXPECT_TRUE(mgr.validate_access(0x2000, 64));
}

TEST(SharedMemoryTest, GuestBufferValidatePartialRangeWorks) {
    GuestBufferManager mgr;
    auto* buf = mgr.allocate_guest_buffer(64, 0x3000);
    ASSERT_NE(buf, nullptr);
    EXPECT_TRUE(mgr.validate_access(0x3010, 16));
}

TEST(SharedMemoryTest, GuestBufferFreeRemovesEntry) {
    GuestBufferManager mgr;
    auto* buf = mgr.allocate_guest_buffer(64, 0x4000);
    ASSERT_NE(buf, nullptr);
    mgr.free_guest_buffer(0x4000);
    EXPECT_FALSE(mgr.validate_access(0x4000, 1));
}
