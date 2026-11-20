#include <gtest/gtest.h>
#include "hypervisor/memory_manager.hpp"

using namespace domvirt::hypervisor;

TEST(MemoryManagerTest, AllocateRam) {
    MemoryManager mm;
    auto result = mm.allocate_ram(64 * 1024 * 1024);
    ASSERT_TRUE(result);
    EXPECT_GT(mm.total_ram(), 0u);
    EXPECT_EQ(mm.slots().size(), 1u);

    // A successful allocation must be usable even when huge pages are unavailable.
    auto* ptr = mm.translate(0);
    ASSERT_NE(ptr, nullptr);
    ptr[0] = 0x5A;
    EXPECT_EQ(ptr[0], 0x5A);
}

TEST(MemoryManagerTest, TranslateValid) {
    MemoryManager mm;
    mm.allocate_ram(4096, 0x1000);
    auto* ptr = mm.translate(0x1000);
    EXPECT_NE(ptr, nullptr);
}

TEST(MemoryManagerTest, TranslateInvalid) {
    MemoryManager mm;
    mm.allocate_ram(4096, 0);
    auto* ptr = mm.translate(0x999999);
    EXPECT_EQ(ptr, nullptr);
}

TEST(MemoryManagerTest, GetSpan) {
    MemoryManager mm;
    mm.allocate_ram(4096, 0);
    auto span = mm.get_span(0, 128);
    EXPECT_EQ(span.size(), 128u);
}

TEST(MemoryManagerTest, RegisterAllSlots) {
    MemoryManager mm;
    mm.allocate_ram(4096, 0);
}

TEST(MemoryManagerTest, IdentityPaging) {
    MemoryManager mm;
    ASSERT_TRUE(mm.allocate_ram(16 * 1024 * 1024, 0, false));
    ASSERT_TRUE(mm.setup_identity_paging());
    auto* pml4 = reinterpret_cast<const uint64_t*>(mm.translate(0x9000));
    auto* pdpt = reinterpret_cast<const uint64_t*>(mm.translate(0xA000));
    auto* pd = reinterpret_cast<const uint64_t*>(mm.translate(0xB000));
    ASSERT_NE(pml4, nullptr);
    ASSERT_NE(pdpt, nullptr);
    ASSERT_NE(pd, nullptr);
    EXPECT_EQ(pml4[0] & ~0xFFFULL, 0xA000ULL);
    EXPECT_EQ(pdpt[0] & ~0xFFFULL, 0xB000ULL);
    EXPECT_EQ(pd[0] & ~0x1FFFFFULL, 0ULL);
    EXPECT_EQ(pd[1] & ~0x1FFFFFULL, 0x200000ULL);
}
