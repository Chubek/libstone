#include "test.hpp"
#include "targets/Firestone.hpp"
#include <libisel/GraphCovering.hpp>
#include <libisel/Verify.hpp>
#include <libregalloc/PBQPRegalloc.hpp>
#include <bit>

using namespace flintstone;
namespace {
FirestoneTarget target() {
    FirestoneTarget result;
    result.scheduling.issue_width = 2;
    result.scheduling.resources = {{"alu", 2}};
    result.registers = firestone::regalloc::Machine::uniform(8);
    for (const auto& [pattern, instruction] : std::vector<std::pair<std::string, std::string>>{
            {"parameter", "ARG"}, {"constant", "IMM"}, {"(copy ?x)", "MOV"},
            {"(add ?lhs ?rhs)", "ADD"}, {"(mul ?lhs ?rhs)", "MUL"}}) {
        firestone::isel::Rule rule;
        rule.name = instruction; rule.instruction = instruction;
        rule.pattern = firestone::rewrite::parse(pattern);
        result.rules.add(rule);
    }
    return result;
}
std::int64_t evaluate_i8(const FirestonePlan& plan, std::int64_t argument) {
    std::map<firestone::isel::NodeId, std::int64_t> values;
    // Execute the selected instructions in verified scheduled order, rather
    // than just comparing two VM compilations of the source modules.
    for (auto id : plan.schedule.order()) {
        const auto& instruction = plan.selection.instructions.at(id);
        std::uint64_t value = 0;
        if (instruction.opcode == "ARG") value = static_cast<std::uint64_t>(argument);
        else if (instruction.opcode == "IMM") {
            const auto& source = plan.region.graph.node(instruction.result).source;
            CHECK(source && source->operands.size() == 1);
            value = static_cast<std::uint64_t>(source->operands.front().integer);
        } else {
            value = static_cast<std::uint64_t>(values.at(instruction.inputs.at(0)));
            if (instruction.opcode == "ADD") value += static_cast<std::uint64_t>(values.at(instruction.inputs.at(1)));
            else if (instruction.opcode == "MUL") value *= static_cast<std::uint64_t>(values.at(instruction.inputs.at(1)));
            else CHECK(instruction.opcode == "MOV");
        }
        values[instruction.result] = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(value));
    }
    CHECK(plan.region.roots.size() == 1);
    return values.at(plan.region.roots.front());
}
void plans_and_blackstone() {
    Registry registry;
    auto module = parse(R"(module m level core {
      func @f(%x:i8)->i8 { ^entry:
        %a:i8=arith.mul(%x,3:i8);
        %b:i8=arith.add(%a,0:i8);
        %c:i8=arith.add(%b,%b);
        func.return(%c);
      }
    })");
    // Sparse Flintstone IDs must not be confused with dense selection IDs.
    auto& f = module.function("f");
    f.parameters.front().id = 100;
    f.blocks.front().operations.front().operands.front() = Operand::ref(100);
    auto machine = target();
    const auto before = print(module);
    auto plan = plan_firestone(module, f, registry, machine);
    CHECK(print(module) == before && module.revision == 0);
    CHECK(plan.region.values.count(100) == 1);
    CHECK(plan.selection.instructions.size() == 6);
    CHECK(plan.schedule.start.size() == plan.selection.instructions.size());
    CHECK(plan.pressure.peak >= 2);
    CHECK(firestone::isel::verify(plan.region.graph, machine.rules, plan.region.roots, plan.selection).empty());
    CHECK(firestone::isched::verify(plan.scheduling, machine.scheduling, plan.schedule).empty());
    CHECK(firestone::regalloc::verify(plan.allocation_problem.problem, machine.registers, plan.allocation).empty());
    // Shared input appears twice in the instruction, but is only selected once.
    const auto& add = plan.selection.instructions.back();
    CHECK(add.inputs.size() == 2 && add.inputs[0] == add.inputs[1]);
    CHECK(plan.allocation_problem.sources.size() == plan.selection.instructions.size());
    for (const auto& [node, value] : plan.allocation_problem.values)
        CHECK(plan.allocation_problem.sources.at(value) == node);

    auto optimized = module;
    PassManager passes;
    passes.add(blackstone_pass(BLACKSTONE_KERNEL_DIR, {"add_zero"}));
    CHECK(passes.run(optimized, registry).front().changed);
    auto reduced = plan_firestone(optimized, optimized.function("f"), registry, machine);
    CHECK(reduced.allocation.feasible);
    for (auto x : {-128, -1, 0, 1, 42, 127}) {
        auto expected = static_cast<std::uint8_t>(x * 6);
        CHECK(run(module, registry, "f", {std::int64_t(x)}) == std::bit_cast<std::int8_t>(expected));
        CHECK(run(module, registry, "f", {std::int64_t(x)}) == run(optimized, registry, "f", {std::int64_t(x)}));
        CHECK(evaluate_i8(plan, x) == run(module, registry, "f", {std::int64_t(x)}));
        CHECK(evaluate_i8(reduced, x) == evaluate_i8(plan, x));
    }
    lower(optimized, registry, Level::Machine);
    CHECK(plan_firestone(optimized, optimized.function("f"), registry, machine).allocation.feasible);

    // High-level lowering is private and scalar types survive export.
    auto high = example("affine.flint");
    auto text = print(high);
    auto high_plan = plan_firestone(high, high.function("affine"), registry, machine);
    CHECK(high_plan.allocation.feasible && print(high) == text);
    auto literal = parse("module m level core {func @f()->i32 {^e:func.return(42:i32);}}");
    CHECK(plan_firestone(literal, literal.function("f"), registry, machine).selection.instructions.size() == 1);
    auto empty = parse("module m level core {func @f()->void {^e:func.return();}}");
    CHECK(plan_firestone(empty, empty.function("f"), registry, machine).selection.instructions.empty());
}
void policies_and_verification() {
    Registry registry;
    auto module = parse("module m level core {func @f(%x:i32)->i32 {^e:%r:i32=arith.add(%x,1:i32);func.return(%r);}}");
    const auto& function = module.function("f");
    auto machine = target();
    machine.select = [](const auto& graph, const auto& rules, const auto& roots) {
        return firestone::isel::GraphCovering{}.select(graph, rules, roots, 0);
    };
    machine.allocate = [](const auto& problem, const auto& registers) {
        return firestone::regalloc::PBQPRegalloc{}.allocate(problem, registers, 0);
    };
    auto limited = plan_firestone(module, function, registry, machine);
    CHECK(limited.selection.stop == firestone::isel::Stop::SearchLimit && !limited.selection.optimal);
    CHECK(limited.allocation.stop == firestone::regalloc::Stop::SearchLimit && limited.allocation.feasible);
    machine.configure_value = [](const auto&, auto& value) {
        value.fixed = 0; // All temporaries cannot share a fixed register.
    };
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.allocation");
    machine = target();
    machine.registers.registers.clear();
    auto spilled = plan_firestone(module, function, registry, machine);
    CHECK(spilled.allocation.feasible && !spilled.allocation.slots.empty());
    machine.configure_value = [](const auto&, auto& value) { value.spillable = false; };
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.allocation");
    machine = target();
    machine.rules = {};
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.selection");
    machine = target();
    machine.select = [](const auto&, const auto&, const auto&) { return firestone::isel::Selection{}; };
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.selection");
    machine = target();
    machine.scheduling.resources.clear();
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.schedule");
    machine = target();
    machine.schedule = [](const auto&, const auto&) { return firestone::isched::Schedule{}; };
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.schedule");
    machine = target();
    machine.allocate = [](const auto&, const auto&) { return firestone::regalloc::Allocation{}; };
    rejects([&] { plan_firestone(module, function, registry, machine); }, "firestone.allocation");
    CHECK(module.revision == 0);
}
void rejected_regions() {
    Registry registry;
    const std::pair<const char*, const char*> cases[] = {
        {"module m level core {extern func @f()->i32;}", "firestone.region"},
        {"module m level core {func @f()->i32 {^e:cf.jump() to [^b];^b:func.return(0:i32);}}", "firestone.region"},
        {"module m level core {func @f()->i32 {^e:%x:i32=arith.div(1:i32,0:i32);func.return(0:i32);}}", "firestone.effect"},
        {"module m level core {func @f(%p:ptr<generic>)->i32 {^e:%x:i32=mem.load(%p);func.return(0:i32);}}", "firestone.effect"},
        {"module m level core {extern func @host()->i32;func @f()->i32 {^e:%x:i32=func.call() attrs{callee=\"host\";};func.return(0:i32);}}", "firestone.effect"},
        {"module m level core {func @f()->i32 {^e:func.return(@global:i32);}}", "firestone.symbol"},
    };
    for (auto [text, diagnostic] : cases) {
        auto module = parse(text);
        const auto before = print(module);
        rejects([&] { plan_firestone(module, module.function("f"), registry, target()); }, diagnostic);
        CHECK(print(module) == before && module.revision == 0);
    }
    auto module = example("affine.flint");
    auto copy = module.function("affine");
    rejects([&] { firestone_region(module, copy, registry); }, "firestone.function");
}
}
int main() {
    try { plans_and_blackstone(); policies_and_verification(); rejected_regions(); std::cout << "Flintstone Firestone integration passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
