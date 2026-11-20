#include <gtest/gtest.h>
#include "bindlet_bridge/lazy_register.hpp"

using namespace domvirt::bindlet_bridge;

TEST(LazyRegisterProxyTest, InitiallyClean) {
    LazyRegisterProxy proxy;
    EXPECT_FALSE(proxy.is_dirty(RegisterClass::GPR));
    EXPECT_FALSE(proxy.is_dirty(RegisterClass::CONTROL));
    EXPECT_FALSE(proxy.is_dirty(RegisterClass::ALL));
}

TEST(LazyRegisterProxyTest, MarkDirty) {
    LazyRegisterProxy proxy;
    proxy.mark_dirty(RegisterClass::GPR);
    EXPECT_TRUE(proxy.is_dirty(RegisterClass::GPR));
    EXPECT_FALSE(proxy.is_dirty(RegisterClass::CONTROL));
}

TEST(LazyRegisterProxyTest, MarkClean) {
    LazyRegisterProxy proxy;
    proxy.mark_dirty(RegisterClass::ALL);
    proxy.mark_clean(RegisterClass::ALL);
    EXPECT_FALSE(proxy.is_dirty(RegisterClass::ALL));
}

TEST(LazyRegisterProxyTest, ModifyRegsMarksDirty) {
    LazyRegisterProxy proxy;
    proxy.regs().rax = 0xDEAD;
    EXPECT_TRUE(proxy.is_dirty(RegisterClass::GPR));
}
