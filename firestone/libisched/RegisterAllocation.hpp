#pragma once
#include "Common.hpp"
#include "../libregalloc/Common.hpp"
namespace firestone::isched {
struct AllocationProblem {
    regalloc::Problem problem;
    std::map<isel::NodeId, regalloc::ValueId> values;
    std::vector<isel::NodeId> sources; // Indexed by allocation value ID.
};
// Values reserve destinations at issue and remain live through completion and
// their last read. Read slots precede destination reservation slots each cycle.
inline AllocationProblem allocation_problem(const isel::Selection& selection,
                                            const Schedule& schedule, const Machine& machine = {},
                                            const std::function<void(const isel::Instruction&, regalloc::Value&)>& configure = {}) {
    const auto graph = from_selection(selection);
    const auto errors = verify(graph, machine, schedule);
    if (!errors.empty()) throw std::invalid_argument(errors.front().message);
    auto read_position = [](Cycle cycle) {
        if (cycle > (std::numeric_limits<regalloc::Position>::max() - 1) / 2)
            throw std::overflow_error("allocation position overflow");
        return cycle * 2;
    };
    AllocationProblem out;
    std::vector<regalloc::Value> values(graph.size());
    for (NodeId id = 0; id < graph.size(); ++id) {
        const auto& instruction = selection.instructions[id];
        out.values.emplace(instruction.result, id); out.sources.push_back(instruction.result);
        auto& value = values[id];
        value.ranges = {{read_position(schedule.start[id]) + 1,
                         read_position(checked_add(schedule.start[id], instruction.latency)) + 1}};
    }
    for (NodeId id = 0; id < graph.size(); ++id) for (auto input : selection.instructions[id].inputs) {
        auto& value = values[out.values.at(input)]; const auto position = read_position(schedule.start[id]);
        value.uses.push_back(position); value.ranges.front().end = std::max(value.ranges.front().end, position + 1);
    }
    // Requested outputs remain available across the whole scheduled region.
    const auto exit = read_position(schedule.makespan);
    for (auto root : selection.roots) {
        auto& value = values[out.values.at(root)];
        value.uses.push_back(exit); value.ranges.front().end = std::max(value.ranges.front().end, exit + 1);
    }
    for (NodeId id = 0; id < values.size(); ++id) {
        auto& value = values[id]; std::sort(value.uses.begin(), value.uses.end());
        value.spill_cost = static_cast<double>(value.uses.size()) + 1;
        if (configure) {
            const auto ranges = value.ranges; const auto uses = value.uses;
            configure(selection.instructions[id], value);
            // Target policy may set classes, precolors, costs, etc., but not
            // invalidate the liveness derived from the checked schedule.
            value.ranges = ranges; value.uses = uses;
        }
        out.problem.add(std::move(value));
    }
    return out;
}
} // namespace firestone::isched
