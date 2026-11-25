#pragma once

#include "MemoryLocation.hpp"
#include "MustMay.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace wetstone::alias {

using RegionId = std::uint32_t;

// Caller-supplied storage evidence. IDs denote descriptors, not necessarily
// distinct locations or individual runtime objects. When given, offset is
// an exact byte displacement from the parent's base for every instance.
struct Region {
    RegionId id = 0;
    std::optional<RegionId> parent;
    std::optional<std::int64_t> offset;
    std::optional<std::uint64_t> size;
    // True only if all queried observations of this descriptor have the same
    // byte base. A base ValueId, if supplied, denotes that invariant address
    // within the same IR/points-to universe, not an allocation site id.
    bool single_instance = false;
    ir::ValueId base = ir::invalid_value;
};

struct RegionAccess {
    RegionId region = 0;
    std::optional<std::int64_t> offset = 0;  // Relative to this region's base.
    std::optional<std::uint64_t> size;
};

// Equal means exactly the same nonempty byte range; Overlap means definite
// intersection of unequal ranges. Empty accesses are Disjoint.
enum class RegionRelation { Unknown, Disjoint, Equal, Overlap };

class RegionBasedAnalysis {
public:
    // Parents can be registered later. Duplicate IDs and cycles are rejected
    // without mutating existing descriptors.
    bool add_region(const Region&);

    // Trusted evidence of disjoint storage for all instances of both regions.
    // Different IDs/names alone do not establish this property. If this
    // contradicts an applicable checked region relation, the query is Unknown.
    bool declare_disjoint(RegionId, RegionId);

    // Find the outermost known invariant pointer anchor along a checked
    // containment chain. No fabricated pointer id or alias proof is returned
    // if there is no such anchor.
    [[nodiscard]] MemoryLocation memory_location(const RegionAccess&) const;
    [[nodiscard]] RegionRelation relationship(const RegionAccess&, const RegionAccess&) const;
    [[nodiscard]] AliasResult alias(const RegionAccess&, const RegionAccess&,
                                    const pointer::PointsToResult& = {}) const;

private:
    struct Projection {
        RegionId region;
        ByteRange range;
    };
    [[nodiscard]] std::vector<Projection> project(const RegionAccess&) const;
    [[nodiscard]] bool has_ancestor(RegionId region, RegionId possible_ancestor) const;
    // nullopt denotes mutually inconsistent region evidence.
    [[nodiscard]] std::optional<RegionRelation> region_relation(const RegionAccess&,
                                                               const RegionAccess&) const;

    std::map<RegionId, Region> regions_;
    std::set<std::pair<RegionId, RegionId>> disjoint_;
};

} // namespace wetstone::alias
