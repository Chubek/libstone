#include "alias-analysis/DirectAnalysis.hpp"
#include "alias-analysis/LoopDependenceAnalysis.hpp"
#include "alias-analysis/RegionBasedAnalysis.hpp"
#include <algorithm>
#include <cassert>
#include <limits>

using namespace wetstone;
using ir::Opcode;
using ir::Operand;
namespace aa = wetstone::alias;

namespace {

ir::Instruction inst(Opcode opcode, std::optional<ir::ValueId> result = {},
                     std::vector<Operand> args = {}, std::vector<ir::BlockId> targets = {},
                     std::string symbol = {}) {
    return {opcode, result, args, targets, {}, symbol, 0};
}

const aa::Dependence* find(const aa::DependenceResult& result, ir::BlockId source_block,
                           std::size_t source, ir::BlockId sink_block, std::size_t sink,
                           aa::DependenceKind kind, aa::IterationRelation iteration) {
    const auto found = std::find_if(result.dependences.begin(), result.dependences.end(),
        [&](const aa::Dependence& dep) {
            return dep.source.block == source_block && dep.source.instruction == source &&
                dep.sink.block == sink_block && dep.sink.instruction == sink &&
                dep.kind == kind && dep.iteration == iteration;
        });
    return found != result.dependences.end() ? &*found : nullptr;
}

void test_direct() {
    const auto a = Operand::ref(1), b = Operand::ref(2);
    const ir::Function f{"direct", 0, {}, {
        {0, {inst(Opcode::AddressOf, 1, {}, {}, "a"), inst(Opcode::AddressOf, 2, {}, {}, "b"),
             inst(Opcode::Store, {}, {a, Operand::constant(1)}), inst(Opcode::Load, 3, {a}),
             inst(Opcode::Load, 4, {b}), inst(Opcode::Call, {}, {}, {}, "reader"),
             inst(Opcode::Call, {}, {}, {}, "pure"), inst(Opcode::Call),
             inst(Opcode::Free, {}, {a}), inst(Opcode::Return)}},
        {1, {inst(Opcode::Call, {}, {}, {}, "unreachable")}}
    }};
    const aa::DirectAnalysis direct(f, {{"reader", {.reads = true}}, {"pure", {}}});
    assert(direct.reachability(0) == aa::Reachability::Reachable);
    assert(direct.reachability(1, 0) == aa::Reachability::Unreachable);
    assert(direct.reachability(0, 11) == aa::Reachability::Unknown);
    assert(direct.calls().size() == 3);
    const auto& accesses = direct.accesses();
    assert(accesses.size() == 6);
    assert(!accesses[0].location.size);
    assert(accesses[3].instruction == 5 && accesses[3].kind == aa::AccessKind::Read);
    assert(accesses[4].instruction == 7 && accesses[4].kind == aa::AccessKind::Unknown);
    assert(accesses[5].instruction == 8 && accesses[5].kind == aa::AccessKind::Unknown);

    const auto conflict = direct.dependences(accesses[0], accesses[1]);
    assert(conflict.complete && conflict.dependences.size() == 1);
    assert(conflict.dependences.front().kind == aa::DependenceKind::Flow);
    assert(conflict.dependences.front().alias == aa::AliasResult::MayAlias);
    assert(direct.dependences(accesses[1], accesses[0]).dependences.empty());
    assert(direct.dependences(accesses[0], accesses[2]).dependences.empty());
    assert(direct.dependences(accesses[1], accesses[3]).dependences.empty());
    assert(direct.dependences(accesses[1], accesses[4]).dependences.size() == 1);
    assert(direct.dependences(accesses[0], accesses[4]).dependences.size() == 2);
    assert(direct.dependences(accesses[4], accesses[4]).dependences.empty());

    const aa::DirectAnalysis bounded(f, {}, 0);
    assert(!bounded.points_to().converged);
    const auto& pending = bounded.accesses();
    assert(!bounded.dependences(pending[0], pending[2]).dependences.empty());
    assert(bounded.alias({1, 0, 1}, {1, 1, 1}) == aa::AliasResult::NoAlias);

    auto bad = f;
    bad.blocks[0].instructions.back() = inst(Opcode::Jump, {}, {}, {42});
    const aa::DirectAnalysis invalid(bad);
    assert(!invalid.valid_cfg());
    assert(invalid.reachability(0) == aa::Reachability::Unknown);
    const auto unknown = invalid.dependences(accesses[0], accesses[2]);
    assert(!unknown.complete && !unknown.dependences.empty());

    auto malformed = f;
    malformed.blocks[0].instructions[3].arguments.clear();
    const aa::DirectAnalysis opaque(malformed);
    assert(opaque.accesses()[1].kind == aa::AccessKind::Unknown);
}

void test_loop_redefinition() {
    const auto flag = Operand::ref(1), ptr = Operand::ref(0), varying = Operand::ref(10);
    const ir::Function f{"loop", 0, {0, 1}, {
        {0, {inst(Opcode::Jump, {}, {}, {1})}},
        {1, {inst(Opcode::Input, 10), inst(Opcode::Store, {}, {varying, flag}),
             inst(Opcode::Load, 11, {varying}), inst(Opcode::Store, {}, {ptr, flag}),
             inst(Opcode::Load, 12, {ptr}), inst(Opcode::Branch, {}, {flag}, {1, 2})}},
        {2, {inst(Opcode::Return)}},
        {9, {inst(Opcode::Call), inst(Opcode::Jump, {}, {}, {1})}}
    }};
    const aa::DirectAnalysis direct(f);
    assert(direct.calls().empty());
    const auto loops = path::natural_loops(f);
    assert(loops.size() == 1);
    auto sized = direct.accesses();
    assert(sized.size() == 4);
    for (auto& access : sized) access.location.size = 4;
    sized[1].location.offset = 4;
    sized[3].location.offset = 4;
    const auto result = aa::analyze_loop_dependences(direct, loops.front(), sized);
    assert(result.complete);
    using enum aa::DependenceKind;
    using enum aa::IterationRelation;
    assert(!find(result, 1, 1, 1, 2, Flow, SameIteration));
    const auto* carried = find(result, 1, 1, 1, 2, Flow, LoopCarried);
    assert(carried && !carried->distance && carried->alias == aa::AliasResult::MayAlias);
    assert(!find(result, 1, 3, 1, 4, Flow, SameIteration));
    assert(!find(result, 1, 3, 1, 4, Flow, LoopCarried));
    assert(!find(result, 1, 1, 1, 1, Output, SameIteration));
    assert(find(result, 1, 1, 1, 1, Output, LoopCarried));

    sized[1].location.offset = 0;
    const auto overlap = aa::LoopDependenceAnalysis::analyze(direct, loops.front(), sized);
    const auto* same = find(overlap, 1, 1, 1, 2, Flow, SameIteration);
    assert(same && same->distance == 0 && same->alias == aa::AliasResult::MustAlias);
    carried = find(overlap, 1, 1, 1, 2, Flow, LoopCarried);
    assert(carried && carried->alias == aa::AliasResult::MayAlias);

    sized.push_back({1, 999, aa::AccessKind::Write, {}});
    assert(!aa::analyze_loop_dependences(direct, loops.front(), sized).complete);
}

void test_loop_structure() {
    const auto ptr = Operand::ref(0);
    const auto condition = Operand::ref(1);
    const ir::Function f{"multiple latches", 0, {0, 1}, {
        {0, {inst(Opcode::Jump, {}, {}, {1})}},
        {1, {inst(Opcode::Branch, {}, {condition}, {2, 3})}},
        {2, {inst(Opcode::Store, {}, {ptr, condition}), inst(Opcode::Jump, {}, {}, {1})}},
        {3, {inst(Opcode::Load, 2, {ptr}), inst(Opcode::Jump, {}, {}, {1})}},
        {9, {inst(Opcode::Call), inst(Opcode::Jump, {}, {}, {2})}}
    }};
    for (const auto& loop : path::natural_loops(f)) {
        const auto merged = aa::analyze_loop_dependences(aa::DirectAnalysis(f), loop);
        assert(merged.complete);
        using enum aa::DependenceKind;
        using enum aa::IterationRelation;
        assert(find(merged, 2, 0, 3, 0, Flow, LoopCarried));
        assert(find(merged, 3, 0, 2, 0, Anti, LoopCarried));
        assert(!find(merged, 2, 0, 3, 0, Flow, SameIteration));
        assert(std::none_of(merged.dependences.begin(), merged.dependences.end(),
            [](const auto& dep) { return dep.source.block == 9 || dep.sink.block == 9; }));
    }

    const ir::Function irreducible{"irreducible", 0, {0}, {
        {0, {inst(Opcode::Branch, {}, {ptr}, {1, 2})}},
        {1, {inst(Opcode::Jump, {}, {}, {2})}},
        {2, {inst(Opcode::Jump, {}, {}, {1})}}
    }};
    assert(!aa::analyze_loop_dependences(aa::DirectAnalysis(irreducible), {1, {1, 2}, {}}).complete);

    auto nested = f;
    const auto varying = Operand::ref(10);
    nested.blocks[2].instructions = {
        inst(Opcode::Input, 10), inst(Opcode::Store, {}, {varying, condition}),
        inst(Opcode::Load, 11, {varying}), inst(Opcode::Branch, {}, {condition}, {2, 1})
    };
    const aa::DirectAnalysis nested_analysis(nested);
    auto sized = nested_analysis.accesses();
    sized[0].location.size = 1;
    sized[1].location.size = 1;
    sized[1].location.offset = 1;
    const auto nested_result =
        aa::analyze_loop_dependences(nested_analysis, {1, {1, 2, 3}, {}}, sized);
    assert(nested_result.complete);
    const auto* inner_carried = find(nested_result, 2, 1, 2, 2, aa::DependenceKind::Flow,
                                    aa::IterationRelation::SameIteration);
    assert(inner_carried && inner_carried->alias == aa::AliasResult::MayAlias);
}

void test_regions() {
    aa::RegionBasedAnalysis regions;
    assert(regions.add_region({1, {}, {}, 128, true, 5}));
    assert(regions.add_region({2, 1, 8, 8}));
    assert(regions.add_region({3, 1, 16, 8}));
    assert(regions.add_region({4, 1, 12, 8}));
    assert(regions.add_region({5, 2, 2, 2}));
    assert(regions.add_region({6, 1, {}, 8}));
    assert(regions.add_region({7, 1, 120, 16}));
    assert(regions.add_region({8, 99, 0, 16}));
    assert(regions.add_region({9, {}, {}, 16, false, 9}));
    assert(regions.add_region({10, {}, {}, 128}));
    assert(regions.add_region({11, 10, 1, 8}));
    assert(regions.add_region({12, {}, {}, std::numeric_limits<std::uint64_t>::max(), true}));
    assert(regions.add_region({13, 12, std::numeric_limits<std::int64_t>::max(), 2}));
    assert(regions.add_region({14, 15, 0, 2}));
    assert(!regions.add_region({15, 14, 0, 2}));
    assert(!regions.add_region({1, {}, {}, 128}));
    assert(!regions.declare_disjoint(1, 2));
    assert(!regions.declare_disjoint(1, 99));

    using enum aa::RegionRelation;
    assert(regions.relationship({2, 0, 8}, {3, 0, 8}) == Disjoint);
    assert(regions.relationship({2, 0, 8}, {4, 0, 8}) == Overlap);
    assert(regions.relationship({2, 0, 8}, {1, 8, 8}) == Equal);
    assert(regions.relationship({5, 0, 2}, {1, 10, 2}) == Equal);
    assert(regions.relationship({2, 0, 8}, {2, 8, 1}) == Unknown);
    assert(regions.relationship({2, 0, 8}, {6, 0, 1}) == Unknown);
    assert(regions.relationship({2, 0, 8}, {7, 0, 1}) == Unknown);
    assert(regions.relationship({2, 0, 8}, {8, 0, 1}) == Unknown);
    assert(regions.relationship({2, 0, 8}, {2, {}, 1}) == Unknown);
    assert(regions.relationship({9, 0, 1}, {9, 1, 1}) == Unknown);
    assert(regions.relationship({9, 0, 1}, {9, 0, 1}) == Unknown);
    assert(regions.relationship({2, 0, 8}, {11, 0, 1}) == Unknown);
    assert(regions.relationship({12, 0, 2}, {13, 0, 2}) == Unknown);
    assert(regions.relationship({2, 0, 8}, {99, {}, 0}) == Disjoint);

    const auto location = regions.memory_location({5, 1, 1});
    assert(location.pointer == 5 && location.offset == 11 && location.size == 1);
    assert(regions.memory_location({9, 0, 1}).pointer == ir::invalid_value);
    assert(regions.alias({2, 0, 8}, {4, 0, 8}) == aa::AliasResult::MustAlias);
    assert(regions.alias({9, 0, 1}, {9, 1, 1}) == aa::AliasResult::MayAlias);
    assert(regions.declare_disjoint(1, 10));
    assert(regions.alias({2, 0, 8}, {11, 0, 1}) == aa::AliasResult::NoAlias);
    assert(regions.alias({6, 0, 8}, {11, 0, 1}) == aa::AliasResult::MayAlias);

    assert(regions.declare_disjoint(2, 4)); // Conflicts with the common-parent ranges.
    assert(regions.relationship({2, 0, 8}, {4, 0, 8}) == Unknown);
    assert(regions.alias({2, 0, 8}, {4, 0, 8}) == aa::AliasResult::MayAlias);
}

} // namespace

int main() {
    test_direct();
    test_loop_redefinition();
    test_loop_structure();
    test_regions();
}
