#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "boot/acpi.hpp"
#include "test_helpers.hpp"
#include "virtio/virtqueue.hpp"

using namespace domvirt::boot;
using namespace domvirt::test_support;
using namespace domvirt::virtio;

namespace {

struct AvailRingTest {
    std::uint16_t flags;
    std::uint16_t idx;
    std::uint16_t ring[8];
};

struct UsedRingTest {
    std::uint16_t flags;
    std::uint16_t idx;
    struct {
        std::uint32_t id;
        std::uint32_t len;
    } ring[8];
};

struct QueueFixture {
    std::vector<std::uint8_t> mem;
    std::uint64_t desc_base{0x1000};
    std::uint64_t avail_base{0x2000};
    std::uint64_t used_base{0x3000};

    QueueFixture() : mem(make_buffer(0x4000, 0)) {}

    VirtioDescriptor* desc(std::size_t index) {
        return reinterpret_cast<VirtioDescriptor*>(mem.data() + desc_base + index * sizeof(VirtioDescriptor));
    }

    AvailRingTest* avail() {
        return reinterpret_cast<AvailRingTest*>(mem.data() + avail_base);
    }

    UsedRingTest* used() {
        return reinterpret_cast<UsedRingTest*>(mem.data() + used_base);
    }
};

void write_descriptor(QueueFixture& q, std::size_t index, std::uint64_t addr,
                      std::uint32_t len, std::uint16_t flags, std::uint16_t next) {
    auto* d = q.desc(index);
    d->addr = addr;
    d->len = len;
    d->flags = flags;
    d->next = next;
}

void write_u16(std::uint8_t* ptr, std::uint16_t value) {
    std::memcpy(ptr, &value, sizeof(value));
}

void write_u32(std::uint8_t* ptr, std::uint32_t value) {
    std::memcpy(ptr, &value, sizeof(value));
}

void write_u64(std::uint8_t* ptr, std::uint64_t value) {
    std::memcpy(ptr, &value, sizeof(value));
}

} // namespace

TEST(VirtqueueTest, RejectsZeroSize) {
    Virtqueue vq(0);
    EXPECT_FALSE(vq.init(0x1000, 0x2000, 0x3000));
}

TEST(VirtqueueTest, RejectsNonPowerOfTwo) {
    Virtqueue vq(3);
    EXPECT_FALSE(vq.init(0x1000, 0x2000, 0x3000));
}

TEST(VirtqueueTest, RejectsOversizedQueue) {
    Virtqueue vq(2048);
    EXPECT_FALSE(vq.init(0x1000, 0x2000, 0x3000));
}

TEST(VirtqueueTest, AcceptsPowerOfTwoQueue) {
    Virtqueue vq(4);
    EXPECT_TRUE(vq.init(0x1000, 0x2000, 0x3000));
    EXPECT_TRUE(vq.is_ready());
}

TEST(VirtqueueTest, AvailNotifyFalseWhenNotReady) {
    Virtqueue vq(4);
    QueueFixture q;
    EXPECT_FALSE(vq.avail_notify(q.mem.data()));
}

TEST(VirtqueueTest, AvailNotifyTrueWhenAvailable) {
    Virtqueue vq(4);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    q.avail()->idx = 1;
    EXPECT_TRUE(vq.avail_notify(q.mem.data()));
}

TEST(VirtqueueTest, ProcessSingleDescriptor) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    q.mem[0x1800] = 0xAA;
    q.mem[0x1801] = 0xBB;
    q.mem[0x1802] = 0xCC;
    q.mem[0x1803] = 0xDD;
    write_descriptor(q, 0, 0x1800, 4, 0, 0);
    q.avail()->idx = 1;
    q.avail()->ring[0] = 0;
    int calls = 0;
    EXPECT_TRUE(vq.process_descriptors(q.mem.data(), [&](std::span<std::uint8_t> span, bool is_write) {
        ++calls;
        EXPECT_FALSE(is_write);
        EXPECT_EQ(span.size(), 4u);
        EXPECT_EQ(span[0], 0xAA);
    }));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(q.used()->idx, 1u);
}

TEST(VirtqueueTest, ProcessDescriptorChain) {
    Virtqueue vq(4);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    q.mem[0x1900] = 1;
    q.mem[0x1901] = 2;
    q.mem[0x1910] = 3;
    q.mem[0x1911] = 4;
    write_descriptor(q, 0, 0x1900, 2, VIRTQ_DESC_F_NEXT, 1);
    write_descriptor(q, 1, 0x1910, 2, VIRTQ_DESC_F_WRITE, 0);
    q.avail()->idx = 1;
    q.avail()->ring[0] = 0;
    int calls = 0;
    bool saw_write = false;
    EXPECT_TRUE(vq.process_descriptors(q.mem.data(), [&](std::span<std::uint8_t> span, bool is_write) {
        ++calls;
        saw_write = is_write;
        EXPECT_EQ(span.size(), 2u);
    }));
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(saw_write);
}

TEST(VirtqueueTest, ProcessDescriptorChainPreservesOrder) {
    Virtqueue vq(4);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    q.mem[0x1A00] = 10;
    q.mem[0x1B00] = 20;
    write_descriptor(q, 0, 0x1A00, 1, VIRTQ_DESC_F_NEXT, 1);
    write_descriptor(q, 1, 0x1B00, 1, 0, 0);
    q.avail()->idx = 1;
    q.avail()->ring[0] = 0;
    std::vector<std::uint64_t> seen;
    EXPECT_TRUE(vq.process_descriptors(q.mem.data(), [&](std::span<std::uint8_t> span, bool) {
        seen.push_back(span.data() - q.mem.data());
    }));
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_LT(seen[0], seen[1]);
}

TEST(VirtqueueTest, ProcessHandlesUnalignedGuestBase) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));

    std::vector<std::uint8_t> backing(q.mem.size() + 1, 0);
    std::memcpy(backing.data() + 1, q.mem.data(), q.mem.size());
    auto* base = backing.data() + 1;

    write_u64(base + q.desc_base + 0, 0x1800);
    write_u32(base + q.desc_base + 8, 4);
    write_u16(base + q.desc_base + 12, 0);
    write_u16(base + q.desc_base + 14, 0);
    write_u16(base + q.avail_base + 0, 0);
    write_u16(base + q.avail_base + 2, 1);
    write_u16(base + q.avail_base + 4, 0);
    write_u16(base + q.used_base + 0, 0);
    write_u16(base + q.used_base + 2, 0);

    base[0x1800] = 0x11;
    base[0x1801] = 0x22;
    base[0x1802] = 0x33;
    base[0x1803] = 0x44;

    bool called = false;
    EXPECT_TRUE(vq.process_descriptors(base, [&](std::span<std::uint8_t> span, bool is_write) {
        called = true;
        EXPECT_FALSE(is_write);
        EXPECT_EQ(span.size(), 4u);
        EXPECT_EQ(span[0], 0x11);
    }));
    EXPECT_TRUE(called);
}

TEST(VirtqueueTest, ResetClearsReadyState) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    vq.reset();
    EXPECT_FALSE(vq.is_ready());
}

TEST(VirtqueueTest, ProcessRejectsOutOfRangeHead) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    q.avail()->idx = 1;
    q.avail()->ring[0] = 3;
    EXPECT_FALSE(vq.process_descriptors(q.mem.data(), [](std::span<std::uint8_t>, bool) {}));
}

TEST(VirtqueueTest, ProcessRejectsDescriptorLoop) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    write_descriptor(q, 0, 0x1800, 1, VIRTQ_DESC_F_NEXT, 1);
    write_descriptor(q, 1, 0x1810, 1, VIRTQ_DESC_F_NEXT, 0);
    q.avail()->idx = 1;
    q.avail()->ring[0] = 0;
    EXPECT_FALSE(vq.process_descriptors(q.mem.data(), [](std::span<std::uint8_t>, bool) {}));
}

TEST(VirtqueueTest, ProcessSupportsWriteDescriptors) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    write_descriptor(q, 0, 0x1C00, 1, VIRTQ_DESC_F_WRITE, 0);
    q.avail()->idx = 1;
    q.avail()->ring[0] = 0;
    bool saw_write = false;
    EXPECT_TRUE(vq.process_descriptors(q.mem.data(), [&](std::span<std::uint8_t>, bool is_write) {
        saw_write = is_write;
    }));
    EXPECT_TRUE(saw_write);
}

TEST(VirtqueueTest, SignalUsedAdvancesUsedIndex) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    EXPECT_EQ(q.used()->idx, 0u);
    vq.add_used(0, 4);
    vq.signal_used(q.mem.data());
    EXPECT_EQ(q.used()->idx, 1u);
}

TEST(VirtqueueTest, NullCallbackFails) {
    Virtqueue vq(2);
    QueueFixture q;
    ASSERT_TRUE(vq.init(q.desc_base, q.avail_base, q.used_base));
    q.avail()->idx = 1;
    q.avail()->ring[0] = 0;
    EXPECT_FALSE(vq.process_descriptors(q.mem.data(), {}));
}
