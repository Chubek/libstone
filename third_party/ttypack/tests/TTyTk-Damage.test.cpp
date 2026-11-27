#include <TTyTk/TTyTk-Damage.hpp>
#include <cassert>
#include <limits>

int main() {
    ttytk::Damage damage;
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    damage.mark(2, max, max);
    damage.mark(1, 4, 2);
    damage.mark(1, 7, 8);
    damage.mark(1, 5, 6);
    damage.mark(2, max - 1, max);
    damage.merge();
    assert((damage.regions() == std::vector<ttytk::DamageRegion>{{1, 2, 8}, {2, max - 1, max}}));
    damage.mark_all(2, 3);
    assert((damage.regions() == std::vector<ttytk::DamageRegion>{{0, 0, 2}, {1, 0, 2}}));
    damage.mark_all(1, 0);
    assert(damage.regions().empty());
    damage.mark(0, 0, 0);
    damage.clear();
    assert(damage.regions().empty());
}
