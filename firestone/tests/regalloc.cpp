#include "Test.hpp"
#include "../libregalloc/BipartiteMatchingRegalloc.hpp"
#include "../libregalloc/BriggsRegalloc.hpp"
#include "../libregalloc/ChaitinRegalloc.hpp"
#include "../libregalloc/CoalescingRegalloc.hpp"
#include "../libregalloc/ILPRegalloc.hpp"
#include "../libregalloc/IntervalSplittingRegalloc.hpp"
#include "../libregalloc/KempeChainRegalloc.hpp"
#include "../libregalloc/LocalNextUseRegalloc.hpp"
#include "../libregalloc/ParallelCopyResolution.hpp"
#include "../libregalloc/RegisterPressure.hpp"
#include "../libregalloc/RegisterScavenging.hpp"
#include "../libregalloc/SimulatedAnnealingRegalloc.hpp"
#include "../libregalloc/SpillSlotColoring.hpp"
#include "../libregalloc/SSABasedLS.hpp"
#include "../libregalloc/SSABasedSpilling.hpp"
#include "../libregalloc/SSAChordalRegalloc.hpp"
using namespace firestone::regalloc;
static Value interval(Position begin, Position end, double cost = 1) {
    Value v; v.ranges = {{begin, end}}; v.spill_cost = cost; return v;
}
static std::size_t spills(const Allocation& a) {
    return std::count_if(a.locations.begin(), a.locations.end(), [](auto l) { return l.kind == LocationKind::Stack || l.kind == LocationKind::Rematerialize; });
}
// Independent exhaustive oracle: register choices or a spill, including copy penalties.
static double oracle(const Problem& p, const Machine& m) {
    std::vector<int> assigned(p.size(), -1); double best = std::numeric_limits<double>::infinity();
    std::function<void(ValueId, double)> visit = [&](ValueId v, double cost) {
        if (v == p.size()) {
            for (auto a : p.affinities()) if (assigned[a.a] < 0 || assigned[a.a] != assigned[a.b]) cost += a.weight;
            best = std::min(best, cost); return;
        }
        const auto& x = p.value(v);
        if (x.spillable && !x.fixed) {
            assigned[v] = -1; visit(v + 1, cost + (x.rematerialization_cost ? std::min(x.spill_cost, *x.rematerialization_cost) : x.spill_cost));
        }
        for (RegisterId r = 0; r < m.registers.size(); ++r) if (legal(x, m, r)) {
            bool ok = true;
            for (ValueId n = 0; n < v; ++n) if (assigned[n] >= 0 && p.interferes(n, v) && m.aliases(r, static_cast<RegisterId>(assigned[n]))) ok = false;
            if (ok) { assigned[v] = static_cast<int>(r); visit(v + 1, cost + m.registers[r].cost); }
        }
        assigned[v] = -1;
    };
    visit(0, 0); return best;
}
int main() { return test_main([] {
    auto machine = Machine::uniform(2);
    Problem triangle; triangle.add(interval(0, 5, 10)); triangle.add(interval(1, 4, 5)); triangle.add(interval(2, 3, 1));
    for (auto a : {GreedyRegalloc{}.allocate(triangle, machine), ChaitinRegalloc{}.allocate(triangle, machine),
                   BriggsRegalloc{}.allocate(triangle, machine), KempeChainRegalloc{}.allocate(triangle, machine),
                   CoalescingRegalloc{}.allocate(triangle, machine), LinearScanRegalloc{}.allocate(triangle, machine),
                   ExtendedLinearScanRegalloc{}.allocate(triangle, machine), SSAChordalRegalloc{}.allocate(triangle, machine),
                   SimulatedAnnealingRegalloc{}.allocate(triangle, machine, 200, 42)}) {
        CHECK(verify(triangle, machine, a).empty()); CHECK(spills(a) >= 1);
    }
    auto exact = PBQPRegalloc{}.allocate(triangle, machine); CHECK(exact.optimal); CHECK(exact.cost == 1);
    auto matching = BipartiteMatchingRegalloc{}.allocate(triangle, machine); CHECK(matching.optimal); CHECK(matching.cost == 1);
    CHECK(verify(triangle, machine, matching).empty());
    auto limited = PBQPRegalloc{}.allocate(triangle, machine, 0);
    CHECK(limited.stop == Stop::SearchLimit); CHECK(!limited.optimal); CHECK(verify(triangle, machine, limited).empty());
    auto corrupt = exact; corrupt.locations[0] = corrupt.locations[1]; CHECK(!verify(triangle, machine, corrupt).empty());
    corrupt = exact; corrupt.cost += 1; CHECK(!verify(triangle, machine, corrupt).empty());
    Problem empty; auto empty_result = PBQPRegalloc{}.allocate(empty, Machine{}); CHECK(empty_result.optimal); CHECK(empty_result.feasible);
    CHECK(verify(empty, {}, empty_result).empty());
    // Exhaust all four-node graphs and compare exact methods to a separate enumerator.
    for (unsigned mask = 0; mask < 64; ++mask) {
        Problem p;
        for (unsigned v = 0; v < 4; ++v) {
            Value x; x.spill_cost = v + 1;
            if (v == 1) x.allowed = std::set<RegisterId>{0};
            if (v == 2) x.rematerialization_cost = 0.5;
            if (v == 3) x.spillable = false;
            p.add(x);
        }
        unsigned bit = 0;
        for (ValueId a = 0; a < 4; ++a) for (ValueId b = a + 1; b < 4; ++b, ++bit) if (mask & (1U << bit)) p.interfere(a, b);
        p.prefer(0, 2, 2); auto m = machine; m.registers[1].cost = 0.25;
        auto expected = oracle(p, m); auto pbqp = PBQPRegalloc{}.allocate(p, m); auto ilp = ILPRegalloc{}.allocate(p, m);
        CHECK(pbqp.optimal); CHECK(pbqp.cost == expected); CHECK(ilp.cost == expected); CHECK(verify(p, m, pbqp).empty());
        for (auto a : {GreedyRegalloc{}.allocate(p, m), BriggsRegalloc{}.allocate(p, m), CoalescingRegalloc{}.allocate(p, m)})
            if (a.feasible) CHECK(verify(p, m, a).empty());
        if (mask == 63) { auto match = BipartiteMatchingRegalloc{}.allocate(p, m); CHECK(match.cost == expected); CHECK(verify(p, m, match).empty()); }
        // Evaluate the inspectable ILP on the returned assignment.
        auto model = ILPRegalloc{}.formulate(p, m); std::set<std::size_t> selected; double cost = model.constant;
        for (const auto& row : model.exactly_one) {
            std::size_t count = 0;
            for (auto index : row) {
                const auto& variable = model.binary_variables[index]; auto actual = ilp.locations[variable.value];
                if (variable.location == actual || (variable.location.kind == LocationKind::Stack && actual.kind == LocationKind::Stack)) {
                    selected.insert(index); cost += variable.cost; ++count;
                }
            }
            CHECK(count == 1);
        }
        for (auto pair : model.at_most_one) CHECK(!(selected.count(pair.first) && selected.count(pair.second)));
        for (auto product : model.products) if (selected.count(product.left) && selected.count(product.right)) cost -= product.saving;
        CHECK(cost == ilp.cost);
    }
    Problem impossible; Value required; required.spillable = false; impossible.add(required);
    CHECK(PBQPRegalloc{}.allocate(impossible, {}).stop == Stop::Infeasible);
    CHECK(GreedyRegalloc{}.allocate(impossible, {}).stop == Stop::NeedsRepair);
    CHECK(BipartiteMatchingRegalloc{}.allocate(impossible, {}).stop == Stop::Infeasible);
    CHECK(PBQPRegalloc{}.allocate(impossible, {}, 0).stop == Stop::SearchLimit);
    Problem no_color; for (int i = 0; i < 3; ++i) no_color.add(required);
    no_color.interfere(0, 1); no_color.interfere(0, 2); no_color.interfere(1, 2);
    CHECK(PBQPRegalloc{}.allocate(no_color, machine).stop == Stop::Infeasible);
    CHECK(BipartiteMatchingRegalloc{}.allocate(no_color, machine).stop == Stop::Infeasible);
    Problem precolored; Value fixed; fixed.fixed = 0; precolored.add(fixed); precolored.add(fixed); precolored.interfere(0, 1);
    throws<std::invalid_argument>([&] { GreedyRegalloc{}.allocate(precolored, machine); });
    Problem constrained; fixed.spill_cost = 10; constrained.add(fixed); constrained.add(); constrained.interfere(0, 1);
    for (auto allocation : {GreedyRegalloc{}.allocate(constrained, machine), PBQPRegalloc{}.allocate(constrained, machine),
                           BipartiteMatchingRegalloc{}.allocate(constrained, machine), CoalescingRegalloc{}.allocate(constrained, machine, true)}) {
        CHECK(allocation.locations[0] == Location::reg(0)); CHECK(allocation.locations[1] == Location::reg(1));
        CHECK(verify(constrained, machine, allocation).empty());
    }
    Value hinted; hinted.hints = {1}; Problem hints; hints.add(hinted);
    CHECK(GreedyRegalloc{}.allocate(hints, machine).locations[0] == Location::reg(1));
    hinted.allowed = std::set<RegisterId>{}; Problem forbidden; forbidden.add(hinted);
    CHECK(spills(GreedyRegalloc{}.allocate(forbidden, machine)) == 1);
    Value invalid; invalid.spill_cost = -1; throws<std::invalid_argument>([&] { Problem p; p.add(invalid); });
    invalid = interval(3, 2); throws<std::invalid_argument>([&] { Problem p; p.add(invalid); });
    invalid = interval(0, 2); invalid.uses = {2}; throws<std::invalid_argument>([&] { Problem p; p.add(invalid); });
    invalid = {}; invalid.spill_alignment = 3; throws<std::invalid_argument>([&] { Problem p; p.add(invalid); });
    // Aliased subregisters and a two-register tuple share allocation units.
    auto irregular = Machine::uniform(3); irregular.registers[1].units = {0}; irregular.registers[2].units = {0, 1};
    auto alias_result = PBQPRegalloc{}.allocate(triangle, irregular); CHECK(spills(alias_result) == 2); CHECK(verify(triangle, irregular, alias_result).empty());
    CHECK(alias_result.cost == oracle(triangle, irregular));
    throws<std::invalid_argument>([&] { BipartiteMatchingRegalloc{}.allocate(triangle, irregular); });
    Problem classes; Value banked; banked.bank = "vector"; banked.reg_class = "vector"; classes.add(banked);
    irregular.registers[2].bank = "vector"; irregular.registers[2].classes = {"vector"};
    CHECK(GreedyRegalloc{}.allocate(classes, irregular).locations[0] == Location::reg(2));
    irregular.registers[2].reserved = true; CHECK(spills(GreedyRegalloc{}.allocate(classes, irregular)) == 1);
    // Half-open endpoints reuse registers; holes can be occupied by other ranges.
    Problem endpoints; endpoints.add(interval(0, 2)); endpoints.add(interval(2, 4));
    CHECK(spills(LinearScanRegalloc{}.allocate(endpoints, Machine::uniform(1))) == 0);
    Problem holes; auto holed = interval(0, 2); holed.ranges.push_back({6, 8}); holes.add(holed); holes.add(interval(2, 6));
    CHECK(spills(LinearScanRegalloc{}.allocate(holes, Machine::uniform(1))) == 1);
    CHECK(spills(ExtendedLinearScanRegalloc{}.allocate(holes, Machine::uniform(1))) == 0);
    auto pressure = RegisterPressure{}.estimate(holes); CHECK(pressure.peak == 1); CHECK(pressure.peak_by_class.at("general") == 1);
    CHECK(RegisterPressure{}.estimate(triangle).peak == 3);
    for (unsigned seed = 0; seed < 24; ++seed) {
        Problem ranges;
        for (unsigned i = 0; i < 6; ++i) {
            Position begin = (seed * 7 + i * 3) % 13;
            auto value = interval(begin, begin + 1 + (seed + i) % 5, i + 1);
            value.ranges.push_back({begin + 8, begin + 10}); ranges.add(value);
        }
        for (auto allocation : {GreedyRegalloc{}.allocate(ranges, machine), LinearScanRegalloc{}.allocate(ranges, machine),
                               ExtendedLinearScanRegalloc{}.allocate(ranges, machine), SimulatedAnnealingRegalloc{}.allocate(ranges, machine, 100, seed)})
            CHECK(verify(ranges, machine, allocation).empty());
    }
    Problem split_input; split_input.add(interval(0, 10)); auto occupied = interval(5, 10); occupied.fixed = 0; split_input.add(occupied);
    auto split = IntervalSplittingRegalloc{}.allocate(split_input, Machine::uniform(1), {{0, {5}}});
    CHECK(split.split.fragments[0].size() == 2); CHECK(split.transfers.size() == 1);
    CHECK(split.transfers[0].position == 5); CHECK(split.transfers[0].from.kind == LocationKind::Register);
    CHECK(split.transfers[0].to.kind == LocationKind::Stack); CHECK(verify(split.split.problem, Machine::uniform(1), split.allocation).empty());
    throws<std::invalid_argument>([&] { LiveRangeSplitting{}.split(split_input, {{0, {5, 5}}}); });
    throws<std::invalid_argument>([&] { LiveRangeSplitting{}.split(split_input, {{0, {10}}}); });
    // Coalescing contracts copies only when the endpoints do not interfere.
    Problem copies; copies.add(); copies.add(); copies.add(); copies.interfere(0, 2); copies.interfere(1, 2); copies.prefer(0, 1);
    auto contracted = coalesce(copies, machine); CHECK(contracted.problem.size() == 2);
    auto coalesced = CoalescingRegalloc{}.allocate(copies, machine); CHECK(coalesced.locations[0] == coalesced.locations[1]); CHECK(verify(copies, machine, coalesced).empty());
    copies.prefer(0, 2, 10); CHECK(coalesce(copies, machine, true).problem.size() == 2);
    Problem kempe; for (int i = 0; i < 4; ++i) kempe.add();
    kempe.interfere(0, 1); kempe.interfere(0, 2); kempe.interfere(1, 3);
    Allocation seed; seed.locations = {Location::stack(0), Location::reg(0), Location::reg(1), Location::reg(1)};
    seed.slots = {{8, 8}}; seed.feasible = true; seed.cost = 1;
    auto swapped = KempeChainRegalloc{}.recolor(kempe, machine, seed);
    CHECK(spills(swapped) == 0); CHECK(verify(kempe, machine, swapped).empty());
    CHECK(spills(KempeChainRegalloc{}.recolor(kempe, machine, seed, 0)) == 1);
    Problem cycle; for (int i = 0; i < 4; ++i) cycle.add();
    cycle.interfere(0, 1); cycle.interfere(1, 2); cycle.interfere(2, 3); cycle.interfere(3, 0);
    CHECK(!chordal_order(cycle).chordal); throws<std::invalid_argument>([&] { SSAChordalRegalloc{}.allocate(cycle, machine); });
    // Spill-slot coloring must reuse only noninterfering storage with sufficient alignment.
    auto stack = GreedyRegalloc{}.allocate(endpoints, {}); CHECK(stack.slots.size() == 1);
    stack.locations[1] = Location::stack(1); stack.slots.push_back({8, 8});
    CHECK(SpillSlotColoring{}.color(endpoints, {}, stack).slots.size() == 1);
    Problem aligned; auto wide = interval(0, 2); wide.spill_size = 32; wide.spill_alignment = 32;
    aligned.add(wide); aligned.add(interval(2, 4)); auto aligned_stack = GreedyRegalloc{}.allocate(aligned, {});
    CHECK(aligned_stack.slots.size() == 1); CHECK(aligned_stack.slots[0].size == 32); CHECK(aligned_stack.slots[0].alignment == 32);
    auto all_stack = GreedyRegalloc{}.allocate(triangle, {}); CHECK(all_stack.slots.size() == 3);
    corrupt = all_stack; corrupt.locations[1] = corrupt.locations[0]; CHECK(!verify(triangle, {}, corrupt).empty());
    auto scavenged = RegisterScavenging{}.scavenge(triangle, machine, exact, {0, 1}, {}, true);
    CHECK(scavenged.reg.has_value()); CHECK(scavenged.save_restore.size() == 1);
    CHECK(!RegisterScavenging{}.scavenge(triangle, machine, exact, {0, 1}, {}, false).reg);
    CHECK(RegisterScavenging{}.scavenge(triangle, machine, exact, {}, {}, false).save_restore.empty());
    // Execute parallel-copy plans, including cycles, fan-out, and disjoint cycles.
    for (auto moves : std::vector<std::vector<Copy>>{
            {{Location::reg(0), Location::reg(1)}, {Location::reg(1), Location::reg(0)}},
            {{Location::reg(0), Location::reg(1)}, {Location::reg(1), Location::reg(2)}, {Location::reg(2), Location::reg(0)}, {Location::reg(0), Location::reg(3)}},
            {{Location::reg(0), Location::reg(1)}, {Location::reg(1), Location::reg(0)}, {Location::reg(2), Location::reg(3)}, {Location::reg(3), Location::reg(2)}}}) {
        std::map<std::pair<LocationKind, std::size_t>, int> memory;
        for (int i = 0; i < 4; ++i) memory[{LocationKind::Register, static_cast<std::size_t>(i)}] = i + 10;
        auto before = memory; auto sequence = ParallelCopyResolution{}.resolve(moves, Location::stack(0));
        for (auto c : sequence) memory[{c.to.kind, c.to.index}] = memory.at({c.from.kind, c.from.index});
        for (auto c : moves) CHECK(memory.at(std::make_pair(c.to.kind, c.to.index)) == before.at(std::make_pair(c.from.kind, c.from.index)));
    }
    throws<std::invalid_argument>([] { ParallelCopyResolution{}.resolve({{Location::reg(0), Location::reg(1)}}, Location::reg(0)); });
    // Simulate local loads/stores and verify every read sees its original SSA value.
    std::vector<LocalAccess> local{{{}, {0}}, {{0}, {1}}, {{1}, {2}}, {{0, 2}, {}}, {{1}, {}}};
    auto local_result = LocalNextUseRegalloc{}.allocate(local, 3, 2, {0, 1, 2}); CHECK(local_result.feasible);
    std::map<ValueId, int> homes; std::vector<int> registers(2, -1);
    for (std::size_t i = 0; i <= local.size(); ++i) {
        for (auto action : local_result.actions) if (action.instruction == i) {
            if (action.kind == LocalActionKind::Store) homes[action.value] = registers[action.reg];
            else registers[action.reg] = homes.at(action.value);
        }
        if (i == local.size()) break;
        for (auto v : local[i].uses) CHECK(registers[local_result.registers[i].at(v)] == static_cast<int>(v));
        for (auto v : local[i].definitions) registers[local_result.registers[i].at(v)] = static_cast<int>(v);
    }
    CHECK(homes.at(0) == 0); CHECK(homes.at(1) == 1); CHECK(homes.at(2) == 2);
    CHECK(!LocalNextUseRegalloc{}.allocate(local, 3, 1).feasible);
    auto live_in = LocalNextUseRegalloc{}.allocate({{{0}, {1}}, {{1}, {}}}, 2, 2, {1});
    CHECK(live_in.feasible); CHECK(live_in.actions.front().kind == LocalActionKind::Load); CHECK(live_in.actions.front().value == 0);
    CHECK(live_in.actions.back().kind == LocalActionKind::Store); CHECK(live_in.actions.back().value == 1);
    // SSA diamond: phi arguments are edge uses, and spill plans identify those edges.
    firestone::ir::SimpleSSAGraph ssa; auto entry = ssa.cfg().create_block("entry");
    auto left = ssa.cfg().create_block("left"), right = ssa.cfg().create_block("right"), join = ssa.cfg().create_block("join");
    auto param = ssa.parameter(firestone::ir::Type::integer());
    auto condition = ssa.parameter(firestone::ir::Type::boolean());
    ssa.cfg().branch(entry, firestone::ir::Operand::ref(condition), left, right);
    auto a = ssa.define(left, firestone::ir::Opcode::Add, firestone::ir::Type::integer(), {firestone::ir::Operand::ref(param), firestone::ir::Operand::constant(1)});
    ssa.cfg().jump(left, join);
    auto b = ssa.define(right, firestone::ir::Opcode::Add, firestone::ir::Type::integer(), {firestone::ir::Operand::ref(param), firestone::ir::Operand::constant(2)});
    ssa.cfg().jump(right, join);
    auto phi = ssa.phi(join, firestone::ir::Type::integer(), {{left, firestone::ir::Operand::ref(a)}, {right, firestone::ir::Operand::ref(b)}});
    ssa.cfg().return_value(join, firestone::ir::Operand::ref(phi));
    auto analyzed = analyze_ssa(ssa); CHECK(analyzed.liveness.live_out.at(left).count(a)); CHECK(!analyzed.liveness.live_in.at(join).count(a));
    CHECK(!analyzed.problem.interferes(a, b));
    CHECK(verify(analyzed.problem, machine, SSABasedLS{}.allocate(ssa, machine)).empty());
    CHECK(verify(analyzed.problem, machine, SSAChordalRegalloc{}.allocate(ssa, machine)).empty());
    auto plan = SSABasedSpilling{}.plan(ssa, {}); CHECK(plan.allocation.feasible);
    std::size_t edge_loads = 0;
    for (auto action : plan.actions) if (action.successor) { CHECK(*action.successor == join); CHECK(action.block == left || action.block == right); ++edge_loads; }
    CHECK(edge_loads == 2); CHECK(verify(plan.analysis.problem, {}, plan.allocation).empty());
    auto remat = SSABasedSpilling{}.plan(ssa, {}, [&](ValueId v, Value& value) { if (v == a) value.rematerialization_cost = 0; });
    CHECK(remat.allocation.locations[a].kind == LocationKind::Rematerialize);
    for (auto action : remat.actions) if (action.value == a) CHECK(action.kind == SpillActionKind::RematerializeUse);
    // Backedge phi operands stay live out of the latch, never as header-local uses.
    firestone::ir::SimpleSSAGraph loop;
    auto start = loop.cfg().create_block("start"), header = loop.cfg().create_block("header");
    auto latch = loop.cfg().create_block("latch"), exit = loop.cfg().create_block("exit");
    auto initial = loop.parameter(firestone::ir::Type::integer());
    auto cond = loop.parameter(firestone::ir::Type::boolean());
    auto next = loop.reserve_value(firestone::ir::Type::integer());
    loop.cfg().jump(start, header);
    auto induction = loop.phi(header, firestone::ir::Type::integer(), {{start, firestone::ir::Operand::ref(initial)}, {latch, firestone::ir::Operand::ref(next)}});
    loop.cfg().branch(header, firestone::ir::Operand::ref(cond), latch, exit);
    loop.define_reserved(latch, next, firestone::ir::Opcode::Add, {firestone::ir::Operand::ref(induction), firestone::ir::Operand::constant(1)});
    loop.cfg().jump(latch, header); loop.cfg().return_value(exit, firestone::ir::Operand::ref(induction));
    auto loop_analysis = analyze_ssa(loop); CHECK(loop_analysis.liveness.live_out.at(latch).count(next));
    CHECK(!loop_analysis.liveness.live_in.at(header).count(next));
    CHECK(verify(loop_analysis.problem, machine, GreedyRegalloc{}.allocate(loop_analysis.problem, machine)).empty());
    CHECK(verify(loop_analysis.problem, machine, SSABasedLS{}.allocate(loop, machine)).empty());
}); }
