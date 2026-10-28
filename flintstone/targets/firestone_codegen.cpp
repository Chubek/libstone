#include "Firestone.hpp"
#include "../internal.hpp"
#include <libisel/BURS.hpp>
#include <libisel/Verify.hpp>
#include <libisched/ListSched.hpp>
#include <libregalloc/GreedyRegalloc.hpp>

namespace flintstone {
FirestoneRegion firestone_region(const Module& source, const Function& input, const Registry& registry) {
    detail::check(&source.function(input.name) == &input, "firestone.function", "function must be owned by the supplied module");
    auto module = instantiate(source, registry);
    if (module.level == Level::High) lower(module, registry, Level::Core);
    const auto& function = module.function(input.name);
    detail::check(!function.external && function.blocks.size() == 1, "firestone.region",
                  "selection requires a single-block function definition", function.location);
    // Use the shared semantic bridge and verifier, not a second opcode mapping.
    auto ssa = to_ssa(module, function, registry);
    FirestoneRegion result;
    std::map<firestone::ir::ValueId, firestone::isel::NodeId> values;
    const auto definitions = ssa.definitions();
    std::size_t parameter = 0;
    for (const auto& [id, definition] : definitions) if (definition.parameter) {
        auto node = result.graph.add("parameter", {}, ssa.type(id));
        values[id] = node;
        result.values[function.parameters.at(parameter++).id] = node;
    }
    auto operand = [&](const firestone::ir::Operand& value) {
        if (value.kind == firestone::ir::OperandKind::Value) return values.at(value.value);
        detail::check(value.kind != firestone::ir::OperandKind::Symbol, "firestone.symbol",
                      "global reads require an explicit effect-aware adapter", function.location);
        firestone::ir::Instruction literal;
        literal.opcode = firestone::ir::Opcode::Constant; literal.type = value.type;
        literal.operands = {value};
        return result.graph.add("constant", {}, value.type, literal);
    };
    const auto& operations = function.blocks.front().operations;
    const auto& instructions = ssa.cfg().block(ssa.cfg().entry()).instructions;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto& instruction = instructions[index];
        const auto& op = operations.at(index);
        if (instruction.opcode == firestone::ir::Opcode::Return) {
            for (const auto& value : instruction.operands) result.roots.push_back(operand(value));
            continue;
        }
        const auto& definition = registry.operation(op.name);
        detail::check(definition.effect == Effect::Pure && !definition.phi && !definition.terminator && instruction.result,
                      "firestone.effect", "selection region cannot discard effects, traps or control flow: " + op.name, op.location);
        std::vector<firestone::isel::NodeId> inputs;
        // Constants keep their literal in Node::source; other operands become
        // explicit DAG children so patterns and target operand order agree.
        if (instruction.opcode != firestone::ir::Opcode::Constant)
            for (const auto& value : instruction.operands) inputs.push_back(operand(value));
        auto node = result.graph.add(firestone::isel::opcode_name(instruction.opcode),
                                     std::move(inputs), instruction.type, instruction);
        values[*instruction.result] = node;
        result.values[op.results.at(0).id] = node;
    }
    return result;
}

FirestonePlan plan_firestone(const Module& module, const Function& function, const Registry& registry,
                             const FirestoneTarget& target) {
    FirestonePlan result;
    result.region = firestone_region(module, function, registry);
    std::string stage = "selection";
    try {
        const auto& region = result.region;
        result.selection = target.select ? target.select(region.graph, target.rules, region.roots) :
            firestone::isel::BURS{}.select(region.graph, target.rules, region.roots);
        auto selection_errors = firestone::isel::verify(region.graph, target.rules, region.roots, result.selection);
        detail::check(selection_errors.empty(), "firestone.selection", selection_errors.empty() ? "" : selection_errors.front().message);
        stage = "schedule";
        result.scheduling = firestone::isched::from_selection(result.selection);
        result.schedule = target.schedule ? target.schedule(result.scheduling, target.scheduling) :
            firestone::isched::ListSched{}.schedule(result.scheduling, target.scheduling);
        detail::check(result.schedule.stop != firestone::isched::Stop::Infeasible &&
                      !(result.schedule.optimal && result.schedule.stop != firestone::isched::Stop::Complete),
                      "firestone.schedule", "schedule has inconsistent feasibility/optimality status");
        auto schedule_errors = firestone::isched::verify(result.scheduling, target.scheduling, result.schedule);
        detail::check(schedule_errors.empty(), "firestone.schedule", schedule_errors.empty() ? "" : schedule_errors.front().message);
        stage = "allocation";
        result.allocation_problem = firestone::isched::allocation_problem(result.selection, result.schedule,
                                                                         target.scheduling, target.configure_value);
        result.pressure = firestone::regalloc::RegisterPressure{}.estimate(result.allocation_problem.problem);
        result.allocation = target.allocate ? target.allocate(result.allocation_problem.problem, target.registers) :
            firestone::regalloc::GreedyRegalloc{}.allocate(result.allocation_problem.problem, target.registers);
        auto allocation_errors = firestone::regalloc::verify(result.allocation_problem.problem, target.registers, result.allocation);
        detail::check(allocation_errors.empty(), "firestone.allocation", allocation_errors.empty() ? "" : allocation_errors.front());
    } catch (const Error&) { throw; }
    catch (const std::exception& e) { detail::fail("firestone." + stage, e.what(), function.location); }
    return result;
}
} // namespace flintstone
