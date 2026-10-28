#pragma once
#include "../Flintstone.hpp"
#include <libisel/Common.hpp>
#include <libisched/RegisterAllocation.hpp>
#include <libregalloc/RegisterPressure.hpp>

namespace flintstone {
// A non-mutating plan for a scalar, single-block function. This does not emit
// machine bytes or replace the Limestone/Sandstone native encoding adapter.
struct FirestoneRegion {
    firestone::isel::Graph graph;
    std::vector<firestone::isel::NodeId> roots;
    std::map<ValueId, firestone::isel::NodeId> values; // Original Flintstone IDs.
};
FirestoneRegion firestone_region(const Module&, const Function&, const Registry&);

struct FirestoneTarget {
    firestone::isel::Rules rules;
    firestone::isched::Machine scheduling;
    firestone::regalloc::Machine registers;
    // Empty callbacks use BURS, ListSched and GreedyRegalloc respectively.
    // Supply callbacks to use other Firestone algorithms, including bounded
    // exact solvers. Feasible SearchLimit incumbents retain their status.
    std::function<firestone::isel::Selection(const firestone::isel::Graph&,
        const firestone::isel::Rules&, const std::vector<firestone::isel::NodeId>&)> select;
    std::function<firestone::isched::Schedule(const firestone::isched::Graph&,
        const firestone::isched::Machine&)> schedule;
    std::function<firestone::regalloc::Allocation(const firestone::regalloc::Problem&,
        const firestone::regalloc::Machine&)> allocate;
    std::function<void(const firestone::isel::Instruction&, firestone::regalloc::Value&)> configure_value;
};
struct FirestonePlan {
    FirestoneRegion region;
    firestone::isel::Selection selection;
    firestone::isched::Graph scheduling;
    firestone::isched::Schedule schedule;
    firestone::isched::AllocationProblem allocation_problem;
    firestone::regalloc::PressureProfile pressure;
    firestone::regalloc::Allocation allocation;
};
// Specialization/high-to-core lowering is private to the call. Run Blackstone
// passes first when desired. Effects, phis, globals and potentially trapping
// operations are rejected, including unused ones; they are never dropped.
// Target rules must implement Flintstone's modular integer semantics and ABI.
// Stack/rematerialization locations are plans, not materialized spill code.
FirestonePlan plan_firestone(const Module&, const Function&, const Registry&, const FirestoneTarget&);
} // namespace flintstone
