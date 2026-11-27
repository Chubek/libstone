#include <TTyTk/TTyTk-Tabstops.hpp>
#include <cassert>
#include <limits>

int main() {
    ttytk::Tabstops unbounded;
    assert(unbounded.next(1000) == 1008);
    assert(unbounded.prev(1008) == 1000);
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    assert(unbounded.next(max - 1) == max - 1);
    unbounded.set(max);
    assert(unbounded.next(max - 1) == max);
    unbounded.clear_all();
    assert(unbounded.next(4) == 4);

    ttytk::Tabstops tabs{19};
    assert(tabs.next(0) == 8);
    assert(tabs.next(16) == 18);
    assert(tabs.prev(7) == 0);
    tabs.clear(8);
    tabs.set(3);
    assert(tabs.next(0) == 3);
    assert(tabs.next(3) == 16);
    assert(tabs.prev(16) == 3);
    tabs.clear_all();
    assert(tabs.next(4) == 18 && tabs.prev(4) == 0);
    tabs.reset();
    tabs.resize(9);
    assert(tabs.prev(8) == 0 && tabs.next(0) == 8);
}
