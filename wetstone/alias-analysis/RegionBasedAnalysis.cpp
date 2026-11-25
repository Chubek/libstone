#include "RegionBasedAnalysis.hpp"
#include <algorithm>

namespace wetstone::alias {
namespace {

std::pair<RegionId, RegionId> ordered_pair(RegionId a, RegionId b) {
    return {std::min(a, b), std::max(a, b)};
}

// Regions require nonnegative, checked displacements. MemoryLocation's
// ordinary alias API still permits negative pointer-relative offsets.
std::optional<ByteRange> contained_range(std::optional<std::int64_t> offset,
                                         std::optional<std::uint64_t> size,
                                         std::optional<std::uint64_t> bound) {
    if (!offset || *offset < 0 || !size || !bound) return std::nullopt;
    const auto range = checked_range(*offset, *size);
    if (!range || static_cast<std::uint64_t>(range->end) > *bound) return std::nullopt;
    return range;
}

} // namespace

bool RegionBasedAnalysis::has_ancestor(RegionId region, RegionId possible_ancestor) const {
    while (region != possible_ancestor) {
        const auto found = regions_.find(region);
        if (found == regions_.end() || !found->second.parent) return false;
        region = *found->second.parent;
    }
    return true;
}

bool RegionBasedAnalysis::add_region(const Region& region) {
    if (regions_.contains(region.id) ||
        (region.parent && has_ancestor(*region.parent, region.id))) return false;
    regions_.emplace(region.id, region);
    return true;
}

bool RegionBasedAnalysis::declare_disjoint(RegionId a, RegionId b) {
    if (!regions_.contains(a) || !regions_.contains(b) ||
        has_ancestor(a, b) || has_ancestor(b, a)) return false;
    disjoint_.insert(ordered_pair(a, b));
    return true;
}

std::vector<RegionBasedAnalysis::Projection>
RegionBasedAnalysis::project(const RegionAccess& access) const {
    std::vector<Projection> result;
    auto current = regions_.find(access.region);
    if (current == regions_.end()) return result;
    auto range = contained_range(access.offset, access.size, current->second.size);
    if (!range) return result;
    while (true) {
        const auto& region = current->second;
        result.push_back({region.id, *range});
        if (!region.parent) break;
        const auto parent = regions_.find(*region.parent);
        if (parent == regions_.end()) break;
        const auto placement = contained_range(region.offset, region.size, parent->second.size);
        if (!placement) break;
        const auto begin = checked_offset_add(placement->begin, range->begin);
        const auto end = checked_offset_add(placement->begin, range->end);
        if (!begin || !end || !placement->contains({*begin, *end})) break;
        range = ByteRange{*begin, *end};
        current = parent;
    }
    return result;
}

MemoryLocation RegionBasedAnalysis::memory_location(const RegionAccess& access) const {
    MemoryLocation location;
    if (access.size == 0) location.size = 0;
    for (const auto& projection : project(access)) {
        const auto& region = regions_.at(projection.region);
        if (region.single_instance && region.base != ir::invalid_value)
            location = {region.base, projection.range.begin, access.size};
    }
    return location;
}

std::optional<RegionRelation>
RegionBasedAnalysis::region_relation(const RegionAccess& a, const RegionAccess& b) const {
    if (a.size == 0 || b.size == 0) return RegionRelation::Disjoint;
    const auto left = project(a), right = project(b);
    RegionRelation relation = RegionRelation::Unknown;
    for (const auto& x : left) {
        for (const auto& y : right) {
            RegionRelation evidence = RegionRelation::Unknown;
            if (x.region == y.region && regions_.at(x.region).single_instance) {
                if (range_alias(x.range, y.range) == AliasResult::NoAlias)
                    evidence = RegionRelation::Disjoint;
                else if (x.range.begin == y.range.begin && x.range.end == y.range.end)
                    evidence = RegionRelation::Equal;
                else
                    evidence = RegionRelation::Overlap;
            } else if (disjoint_.contains(ordered_pair(x.region, y.region)))
                evidence = RegionRelation::Disjoint;
            if (evidence == RegionRelation::Unknown) continue;
            if (relation != RegionRelation::Unknown && relation != evidence)
                return std::nullopt;
            relation = evidence;
        }
    }
    return relation;
}

RegionRelation RegionBasedAnalysis::relationship(const RegionAccess& a, const RegionAccess& b) const {
    return region_relation(a, b).value_or(RegionRelation::Unknown);
}

AliasResult RegionBasedAnalysis::alias(const RegionAccess& a, const RegionAccess& b,
                                      const pointer::PointsToResult& points_to) const {
    const auto relation = region_relation(a, b);
    if (!relation) return AliasResult::MayAlias;
    const auto ordinary = wetstone::alias::alias(memory_location(a), memory_location(b), points_to);
    if (*relation == RegionRelation::Unknown) return ordinary;
    const auto region_alias = *relation == RegionRelation::Disjoint
        ? AliasResult::NoAlias : AliasResult::MustAlias;
    return ordinary == AliasResult::MayAlias || ordinary == region_alias
        ? region_alias : AliasResult::MayAlias;
}

} // namespace wetstone::alias
