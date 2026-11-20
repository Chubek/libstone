#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "event_queue.hpp"

using namespace domios;

namespace {

Event make_event(EventKind kind, std::uint32_t code, std::string subject, std::string message) {
    return Event{kind, code, std::move(subject), std::move(message)};
}

} // namespace

TEST(DomIOSQueueTest, CapacityZeroBecomesOne) {
    EventQueue queue(0);
    EXPECT_EQ(queue.capacity(), 1u);
}

TEST(DomIOSQueueTest, PushPopFifoOne) {
    EventQueue queue(2);
    EXPECT_TRUE(queue.try_push(make_event(EventKind::log, 1, "a", "one")));
    EXPECT_TRUE(queue.try_push(make_event(EventKind::diagnostic, 2, "b", "two")));
    Event event{};
    EXPECT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.code, 1u);
}

TEST(DomIOSQueueTest, PushPopFifoTwo) {
    EventQueue queue(2);
    queue.try_push(make_event(EventKind::log, 1, "a", "one"));
    queue.try_push(make_event(EventKind::diagnostic, 2, "b", "two"));
    Event event{};
    queue.try_pop(event);
    EXPECT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.code, 2u);
}

TEST(DomIOSQueueTest, EmptyPopFails) {
    EventQueue queue(2);
    Event event{};
    EXPECT_FALSE(queue.try_pop(event));
}

TEST(DomIOSQueueTest, SizeTracksPushes) {
    EventQueue queue(4);
    EXPECT_EQ(queue.size(), 0u);
    queue.try_push(make_event(EventKind::log, 1, "a", "one"));
    EXPECT_EQ(queue.size(), 1u);
}

TEST(DomIOSQueueTest, SizeTracksPops) {
    EventQueue queue(4);
    queue.try_push(make_event(EventKind::log, 1, "a", "one"));
    Event event{};
    queue.try_pop(event);
    EXPECT_EQ(queue.size(), 0u);
}

TEST(DomIOSQueueTest, OverwriteSameKindReturnsFalse) {
    EventQueue queue(1);
    EXPECT_TRUE(queue.try_push(make_event(EventKind::log, 1, "a", "one")));
    EXPECT_FALSE(queue.try_push(make_event(EventKind::log, 2, "b", "two")));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.code, 2u);
    EXPECT_EQ(event.subject, "b");
}

TEST(DomIOSQueueTest, EvictOldestWhenKindsDiffer) {
    EventQueue queue(1);
    queue.try_push(make_event(EventKind::log, 1, "a", "one"));
    EXPECT_TRUE(queue.try_push(make_event(EventKind::diagnostic, 2, "b", "two")));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.kind, EventKind::diagnostic);
    EXPECT_EQ(event.code, 2u);
}

TEST(DomIOSQueueTest, SubjectRoundTrips) {
    EventQueue queue(2);
    queue.try_push(make_event(EventKind::plugin_notice, 7, "plug", "hello"));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.subject, "plug");
}

TEST(DomIOSQueueTest, MessageRoundTrips) {
    EventQueue queue(2);
    queue.try_push(make_event(EventKind::shell_output, 8, "shell", "done"));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.message, "done");
}

TEST(DomIOSQueueTest, InterleavedPushPop) {
    EventQueue queue(2);
    queue.try_push(make_event(EventKind::log, 1, "a", "one"));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.code, 1u);
    EXPECT_TRUE(queue.try_push(make_event(EventKind::domainlet_error, 2, "b", "two")));
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.kind, EventKind::domainlet_error);
}

TEST(DomIOSQueueTest, MultipleKindsRemainDistinct) {
    EventQueue queue(3);
    queue.try_push(make_event(EventKind::log, 1, "a", "one"));
    queue.try_push(make_event(EventKind::diagnostic, 2, "b", "two"));
    queue.try_push(make_event(EventKind::shell_output, 3, "c", "three"));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.kind, EventKind::log);
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.kind, EventKind::diagnostic);
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.kind, EventKind::shell_output);
}

TEST(DomIOSQueueTest, FullQueueOverwriteKeepsNewestPayload) {
    EventQueue queue(1);
    queue.try_push(make_event(EventKind::log, 1, "old", "old"));
    queue.try_push(make_event(EventKind::log, 2, "new", "new"));
    Event event{};
    ASSERT_TRUE(queue.try_pop(event));
    EXPECT_EQ(event.subject, "new");
    EXPECT_EQ(event.message, "new");
}
