#include "Test.hpp"
#include "../libisched/AntColonyOpt.hpp"
#include "../libisched/ConstraintProgSched.hpp"
#include "../libisched/DelaySlotSched.hpp"
#include "../libisched/HyperblockSched.hpp"
#include "../libisched/IntegerProgSched.hpp"
#include "../libisched/LocalSched.hpp"
#include "../libisched/LoopUnrolling.hpp"
#include "../libisched/ModuloSched.hpp"
#include "../libisched/ScoreBoarding.hpp"
#include "../libisched/SelectionDAGSched.hpp"
#include "../libisched/SuperblockSched.hpp"
#include "../libisched/Tomasulo.hpp"
#include "../libisched/VLIWPacking.hpp"
#include "../libisel/DAGDynProg.hpp"
using namespace firestone::isched;
static Operation op(std::string name, Cycle latency = 1, std::string resource = "alu") {
    Operation out; out.opcode = std::move(name); out.latency = latency; out.resource = std::move(resource); return out;
}
int main() { return test_main([] {
    Graph g; auto a = g.add(op("a", 3)), b = g.add(op("b")), c = g.add(op("c"));
    auto d = g.add(op("d")); g.depend(a, c, 3); g.depend(b, d, 1); g.depend(c, d, 1);
    Machine m{2, {{"alu", 2}}};
    auto list = ListSched{}.schedule(g, m); CHECK(list.makespan == 5); CHECK(list.start[a] == 0); CHECK(list.start[b] == 0);
    CHECK(verify(g, m, list).empty());
    CHECK(verify(g, m, LocalSched{}.schedule(g, m)).empty());
    CHECK(verify(g, m, SelectionDAGSched{}.schedule(g, m)).empty());
    CHECK(verify(g, m, ProfileDrivenSched{}.schedule(g, m)).empty());
    CHECK(verify(g, m, AntColonyOpt{}.schedule(g, m, 4, 3, 42)).empty());
    auto integer = IntegerProgSched{}.schedule(g, m); CHECK(integer.optimal); CHECK(integer.makespan == 5);
    CHECK(ConstraintProgSched{}.schedule(g, m).makespan == 5);
    CHECK(VLIWPacking{}.pack(g, m).bundles.front().operations.size() == 2);
    auto invalid = list; invalid.start[c] = 0; CHECK(!verify(g, m, invalid).empty());
    invalid = list; invalid.makespan = 0; CHECK(!verify(g, m, invalid).empty());
    throws<std::invalid_argument>([&] { ListSched{}.schedule(g, {0, {{"alu", 1}}}); });
    throws<std::invalid_argument>([&] { ListSched{}.schedule(g, {1, {}}); });
    Graph cycle; cycle.add(op("a")); cycle.add(op("b")); cycle.depend(0, 1, 1); cycle.depend(1, 0, 1);
    throws<std::logic_error>([&] { ListSched{}.schedule(cycle); });
    Graph occupied; auto long_op = op("busy"); long_op.occupancy = 3;
    occupied.add(long_op); occupied.add(long_op);
    auto packed = ListSched{}.schedule(occupied); CHECK(packed.start[1] == 3); CHECK(verify(occupied, {}, packed).empty());
    // Exhaustive oracle for tiny independent resource-constrained DAGs.
    for (unsigned mask = 0; mask < 8; ++mask) {
        Graph small; small.add(op("a", 2)); small.add(op("b")); small.add(op("c", 2));
        if (mask & 1) small.depend(0, 1, 2);
        if (mask & 2) small.depend(0, 2, 2);
        if (mask & 4) small.depend(1, 2, 1);
        Cycle optimum = 99;
        for (Cycle x = 0; x < 6; ++x) for (Cycle y = 0; y < 6; ++y) for (Cycle z = 0; z < 6; ++z) {
            Schedule candidate; candidate.start = {x,y,z}; candidate.makespan = std::max({x+2,y+1,z+2});
            if (verify(small, {}, candidate).empty()) optimum = std::min(optimum, candidate.makespan);
        }
        auto cp = ConstraintProgSched{}.schedule(small); auto ilp = IntegerProgSched{}.schedule(small);
        CHECK(cp.optimal); CHECK(cp.makespan == optimum); CHECK(ilp.makespan == optimum);
    }
    Graph loop; loop.add(op("recurrence", 3)); loop.depend(0, 0, 3, DependencyKind::Data, 1);
    throws<std::invalid_argument>([&] { ListSched{}.schedule(loop); });
    auto modulo = ModuloSched{}.schedule(loop, {}, 5, 2);
    CHECK(modulo.initiation_interval == 3); CHECK(verify(loop, {}, modulo).empty());
    CHECK(ModuloSched{}.schedule(loop, {}, 2, 2).kernel.stop == Stop::SearchLimit);
    auto unrolled = LoopUnrolling{}.unroll(loop, 2); CHECK(unrolled.graph.size() == 2);
    CHECK(unrolled.graph.dependencies()[0].distance == 0); CHECK(unrolled.graph.dependencies()[1].distance == 1);
    throws<std::invalid_argument>([&] { LoopUnrolling{}.unroll(loop, 0); });
    auto periodic_busy = ModuloSched{}.schedule(occupied, {}, 8, 2);
    CHECK(periodic_busy.initiation_interval == 6); CHECK(verify(occupied, {}, periodic_busy).empty());
    auto dyn = OutOfOrder{}.run(g, m); CHECK(verify(g, m, dyn.execution).empty());
    for (std::size_t i = 1; i < dyn.retire.size(); ++i) CHECK(dyn.retire[i] >= dyn.retire[i-1]);
    auto scoreboard = ScoreBoarding{}.run(g, m); CHECK(verify(g, m, scoreboard.execution).empty());
    CHECK(verify(g, m, Tomasulo{}.run(g, m).execution).empty());
    auto registers = register_dependencies({{op("old", 5), {}, {0}}, {op("new"), {}, {0}}, {op("use"), {0}, {1}}});
    auto score = ScoreBoarding{}.run(registers, m); auto tomasulo = Tomasulo{}.run(registers, m);
    CHECK(tomasulo.execution.start[1] < score.execution.start[1]);
    CHECK(tomasulo.execution.start[2] >= tomasulo.complete[1]);
    Graph speculate; speculate.add(op("branch", 4)); auto pure = op("pure"); pure.speculatable = true;
    speculate.add(pure); speculate.depend(0, 1, 4, DependencyKind::Control);
    auto spec = Speculative{}.schedule(speculate, m); CHECK(spec.relaxed.size() == 1); CHECK(spec.schedule.start[1] == 0);
    CHECK(verify(spec.graph, m, spec.schedule).empty());
    Graph slots; slots.add(pure); slots.add(op("branch"));
    auto delay = DelaySlotSched{}.fill(slots, {0,1}, 1, 2); CHECK(delay.order == std::vector<NodeId>({1,0})); CHECK(delay.slots[0] == 0); CHECK(!delay.slots[1]);
    slots.depend(0, 1, 1); CHECK(!DelaySlotSched{}.fill(slots, {0,1}, 1, 1).slots[0]);
    firestone::ir::SimpleSSAGraph ssa; auto block = ssa.cfg().create_block("entry");
    auto param = ssa.parameter(firestone::ir::Type::integer());
    auto value = ssa.define(block, firestone::ir::Opcode::Add, firestone::ir::Type::integer(), {firestone::ir::Operand::ref(param), firestone::ir::Operand::constant(1)});
    ssa.cfg().return_value(block, firestone::ir::Operand::ref(value));
    auto adapted = from_ssa_block(ssa, block); CHECK(adapted.size() == 2); CHECK(verify(adapted, {}, ListSched{}.schedule(adapted)).empty());
    Graph region; auto first = op("entry"); first.block = 0; region.add(first);
    auto second = pure; second.block = 1; region.add(second);
    auto trace = TraceSched{}.schedule(region, {0,1}, m); CHECK(trace.schedule.start[1] >= 1);
    auto hyper = HyperblockSched{}.schedule(region, {0,1}, m); CHECK(hyper.schedule.start[1] == 0);
    firestone::isel::Graph terms; auto leaf = terms.add("x"); auto root = terms.add("neg", {leaf});
    firestone::isel::Rules rules;
    firestone::isel::Rule xr; xr.name = "x"; xr.pattern = firestone::rewrite::parse("x"); xr.instruction = "X"; rules.add(xr);
    auto nr = xr; nr.name = "neg"; nr.pattern = firestone::rewrite::parse("(neg ?x)"); nr.instruction = "NEG"; rules.add(nr);
    auto selected = firestone::isel::DAGDynProg{}.select(terms, rules, {root}); auto scheduled = from_selection(selected);
    CHECK(scheduled.size() == 2); CHECK(ListSched{}.schedule(scheduled).makespan == 2);
    auto model = IntegerProgSched{}.formulate(g, m, list.makespan);
    for (const auto& row : model.constraints) {
        std::int64_t value_sum = 0;
        for (const auto& term : row.terms) {
            const auto& variable = model.binary_variables[term.first];
            if (list.start[variable.operation] == variable.cycle) value_sum += term.second;
        }
        if (row.relation == Relation::Equal) CHECK(value_sum == row.rhs);
        else if (row.relation == Relation::LessEqual) CHECK(value_sum <= row.rhs);
        else CHECK(value_sum >= row.rhs);
    }
    throws<std::length_error>([&] { IntegerProgSched{}.formulate(g, m, 10, 0); });
    firestone::ir::ControlFlowGraph cfg;
    auto entry = cfg.create_block("entry"), hot = cfg.create_block("hot"), cold = cfg.create_block("cold"), join = cfg.create_block("join");
    cfg.branch(entry, firestone::ir::Operand::constant(1, firestone::ir::Type::boolean()), hot, cold);
    cfg.jump(hot, join); cfg.jump(cold, join); cfg.return_value(join);
    EdgeFrequencies frequencies{{{entry, hot}, 9}, {{entry, cold}, 1}};
    CHECK(TraceSched{}.trace(cfg, entry, frequencies) == std::vector<NodeId>({entry, hot, join}));
    CHECK(SuperblockSched{}.region(cfg, entry, frequencies) == std::vector<NodeId>({entry, hot}));
    auto super = SuperblockSched{}.schedule(region, cfg, {entry, hot}, m);
    CHECK(super.schedule.start[1] == 0); CHECK(verify(super.graph, m, super.schedule).empty());
    throws<std::invalid_argument>([&] { SuperblockSched{}.schedule(region, cfg, {entry, join}, m); });
    throws<std::invalid_argument>([&] { TraceSched{}.trace(cfg, entry, {{{hot, cold}, 1}}); });
    Graph unsafe; unsafe.add(first); second.speculatable = false; unsafe.add(second);
    throws<std::invalid_argument>([&] { HyperblockSched{}.schedule(unsafe, {0,1}, m); });
    Graph predicated; predicated.add(first); second.predicate = "p1"; predicated.add(second);
    predicated.depend(0, 1, 1, DependencyKind::Data); // predicate availability remains a real edge
    CHECK(HyperblockSched{}.schedule(predicated, {0,1}, m).schedule.start[1] == 1);
    throws<std::invalid_argument>([&] { DelaySlotSched{}.fill(slots, {0,0}, 1, 1); });
    throws<std::invalid_argument>([&] { OutOfOrder{}.run(g, m, 0); });
    auto broken_modulo = modulo; broken_modulo.initiation_interval = 1;
    CHECK(!verify(loop, {}, broken_modulo).empty());
    Graph backwards; backwards.add(op("use")); backwards.add(op("def")); backwards.depend(1,0,1);
    throws<std::invalid_argument>([&] { ScoreBoarding{}.run(backwards, m); });
    Graph empty; CHECK(ListSched{}.schedule(empty).start.empty()); CHECK(OutOfOrder{}.run(empty).cycles == 0);
}); }
