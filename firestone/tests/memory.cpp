#include "Test.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <random>
#include <vector>

using namespace firestone::memory;

struct Counted : RefCounted {
    int& destroyed;
    explicit Counted(int& destroyed) : destroyed(destroyed) {}
    ~Counted() override { ++destroyed; }
};
struct Traced {
    int& destroyed;
    explicit Traced(int& destroyed) : destroyed(destroyed) {}
    ~Traced() { ++destroyed; }
};

static void generational_gc() {
    int destroyed = 0;
    GC<std::unique_ptr<Traced>> heap;
    auto allocate = [&] { return heap.allocate(std::make_unique<Traced>(destroyed)); };
    auto old = allocate();
    auto old_peer = allocate();
    heap.set_references(old, {old_peer});
    heap.set_references(old_peer, {old});
    CHECK(heap.collect({old}).retained == 2);

    auto bridge = allocate();
    auto leaf = allocate();
    auto drop_edge = allocate();
    auto dead_cycle = allocate();
    auto pinned = allocate();
    auto explicit_root = allocate();
    heap.set_references(old_peer, {old, bridge, bridge});
    heap.set_references(bridge, {leaf});
    heap.set_references(leaf, {bridge, old});
    heap.set_references(dead_cycle, {dead_cycle, bridge});
    heap.set_references(old, {old_peer, drop_edge});
    heap.set_references(old, {old_peer});
    heap.pin(pinned);
    auto result = heap.collect_young({explicit_root, explicit_root});
    CHECK(result.retained == 6 && result.reclaimed == 2 && destroyed == 2);
    CHECK(heap.contains(old) && heap.contains(old_peer) && heap.contains(bridge) && heap.contains(leaf));
    CHECK(heap.contains(pinned) && heap.contains(explicit_root));
    CHECK(!heap.contains(drop_edge) && !heap.contains(dead_cycle));

    auto after_promotion = allocate();
    CHECK(after_promotion > explicit_root);
    heap.set_references(leaf, {after_promotion});
    throws<std::out_of_range>([&] { heap.set_references(leaf, {dead_cycle}); });
    throws<std::out_of_range>([&] { (void)heap.collect_young({dead_cycle}); });
    CHECK(heap.size() == 7 && heap.references(leaf) == std::vector<Handle>{after_promotion});
    heap.unpin(pinned);
    result = heap.collect_young();
    CHECK(result.retained == 7 && result.reclaimed == 0 && destroyed == 2);
    result = heap.collect_young(); // Empty nursery; old entries are still live.
    CHECK(result.retained == 7 && result.reclaimed == 0 && destroyed == 2);
    result = heap.collect();
    CHECK(result.retained == 0 && result.reclaimed == 7 && destroyed == 9 && heap.size() == 0);
    result = heap.collect_young();
    CHECK(result.retained == 0 && result.reclaimed == 0);
}

static void generational_gc_against_full() {
    GC<int> heap;
    std::mt19937 random(2026);
    std::vector<Handle> alive;
    for (int round = 0; round < 30; ++round) {
        auto roots = alive; // A minor collection treats every old object as a root.
        for (int i = 0; i < 20; ++i) alive.push_back(heap.allocate(i));
        for (auto object : alive) {
            std::vector<Handle> edges;
            for (auto n = random() % 3; n != 0; --n) edges.push_back(alive[random() % alive.size()]);
            heap.set_references(object, std::move(edges));
        }
        auto pinned = alive.back();
        heap.pin(pinned);
        auto young_root = alive[alive.size() - 2];
        roots.push_back(young_root);

        GC<int> reference = heap;
        auto expected = reference.collect(roots);
        // Both copying and moving must preserve the remembered set and roots.
        GC<int> copy;
        copy = heap;
        GC<int> moved(std::move(copy));
        CHECK(copy.size() == 0);
        heap = std::move(moved);
        CHECK(moved.size() == 0);
        auto actual = heap.collect_young({young_root});
        CHECK(actual.retained == expected.retained && actual.reclaimed == expected.reclaimed);
        CHECK(heap.size() == actual.retained);
        for (auto object : alive) CHECK(heap.contains(object) == reference.contains(object));
        heap.unpin(pinned);

        if (round % 5 == 0) (void)heap.collect({young_root});
        alive.erase(std::remove_if(alive.begin(), alive.end(), [&](Handle object) { return !heap.contains(object); }), alive.end());
    }
}

int main() { return test_main([] {
    Allocation aligned(100, 256);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned.data()) % 256 == 0);
    Allocation moved(std::move(aligned));
    CHECK(!aligned.data() && moved.size() == 100);
    throws<std::invalid_argument>([] { Allocation invalid(4, 3); });
    throws<std::overflow_error>([] { align_up(std::numeric_limits<std::size_t>::max(), 8); });

    Pool pool(17, 2, 64);
    auto first = pool.allocate(), second = pool.allocate();
    CHECK(pool.owns(first) && pool.available() == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(first) % 64 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(second) % 64 == 0);
    throws<std::bad_alloc>([&] { pool.allocate(); });
    throws<std::invalid_argument>([&] { pool.deallocate(static_cast<std::byte*>(first) + 1); });
    pool.deallocate(first);
    throws<std::invalid_argument>([&] { pool.deallocate(first); });
    CHECK(pool.allocate() == first);
    PoolResource pooled(pool);
    throws<std::bad_alloc>([&] { (void)pooled.allocate(65); });
    pool.deallocate(second);
    auto storage = pooled.allocate(12, 32); pooled.deallocate(storage, 12, 32);
    throws<std::overflow_error>([] { Pool invalid(64, std::numeric_limits<std::size_t>::max()); });

    Arena arena(32);
    auto bytes = static_cast<std::byte*>(arena.allocate(31)); bytes[0] = std::byte{42};
    auto over_aligned = arena.allocate(100, 128);
    CHECK(reinterpret_cast<std::uintptr_t>(over_aligned) % 128 == 0 && bytes[0] == std::byte{42});
    {
        ArenaResource resource(arena);
        std::pmr::vector<int> values(&resource);
        for (int i = 0; i < 100; ++i) values.push_back(i);
        CHECK(values[42] == 42);
    }
    CHECK(arena.reserved_bytes() >= 131); arena.reset(); CHECK(arena.reserved_bytes() == 0);

    Buffer buffer(3); buffer.at(0) = std::byte{9};
    buffer.append(buffer.data(), buffer.size());
    CHECK(buffer.size() == 6 && buffer.at(3) == std::byte{9});
    Buffer copy = buffer; copy.at(0) = std::byte{1}; CHECK(buffer.at(0) == std::byte{9});
    Buffer transferred = std::move(copy); CHECK(copy.size() == 0 && transferred.size() == 6);
    buffer.resize(8); CHECK(buffer.at(7) == std::byte{0});
    throws<std::out_of_range>([&] { buffer.at(8); });

    int destroyed = 0;
    {
        auto pointer = make_intrusive<Counted>(destroyed);
        auto another = pointer; CHECK(pointer->use_count() == 2);
        another.reset(); pointer.reset(pointer.get()); CHECK(pointer->use_count() == 1);
        auto moved_pointer = std::move(pointer); CHECK(!pointer && moved_pointer);
    }
    CHECK(destroyed == 1);
    destroyed = 0;
    GC<std::unique_ptr<Traced>> heap;
    auto a = heap.allocate(std::make_unique<Traced>(destroyed));
    auto b = heap.allocate(std::make_unique<Traced>(destroyed), {a});
    heap.set_references(a, {b});
    auto garbage = heap.allocate(std::make_unique<Traced>(destroyed));
    heap.pin(a);
    auto collection = heap.collect({a, b, a});
    CHECK(collection.retained == 2 && collection.reclaimed == 1 && destroyed == 1);
    CHECK(!heap.contains(garbage));
    throws<std::out_of_range>([&] { heap.collect({garbage}); });
    CHECK(heap.size() == 2);
    heap.unpin(a); collection = heap.collect();
    CHECK(collection.reclaimed == 2 && destroyed == 3);
    CHECK(heap.allocate(std::make_unique<Traced>(destroyed)) > garbage);

    generational_gc();
    generational_gc_against_full();
}); }
