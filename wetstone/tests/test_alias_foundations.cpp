#include "alias-analysis/CallSite.hpp"
#include "alias-analysis/MustMay.hpp"
#include "alias-analysis/TypeDAG.hpp"

#include <cassert>
#include <limits>
#include <utility>

using namespace wetstone;

namespace {

ir::Instruction inst(ir::Opcode opcode, std::optional<ir::ValueId> result = {},
                     std::vector<ir::Operand> args = {}, std::vector<ir::BlockId> targets = {},
                     std::vector<ir::BlockId> incoming = {}, std::string symbol = {}) {
    return {opcode, result, std::move(args), std::move(targets), std::move(incoming), std::move(symbol), 0};
}

void types() {
    alias::TypeDAG graph;
    const auto top = graph.add_type("top");
    const auto left = graph.add_type("left");
    const auto right = graph.add_type("right");
    const auto bottom = graph.add_type("bottom");
    assert(graph.size() == 4);
    assert(graph.add_type("left") == left);
    assert(graph.find("left") == left && !graph.find("missing"));
    assert(graph.name(right) == "right" && !graph.name(alias::invalid_type));
    assert(graph.add_parent(left, top));
    assert(graph.add_parent(right, top));
    assert(graph.add_parent(bottom, left) && graph.add_parent(bottom, right));
    assert(graph.add_parent(bottom, right));
    assert(graph.parents(bottom).size() == 2);
    assert(graph.is_subtype(bottom, top) && graph.is_subtype(right, right));
    assert(!graph.is_subtype(left, right) && !graph.is_subtype(alias::invalid_type, top));
    assert(!graph.add_parent(top, bottom) && !graph.add_parent(left, left));
    assert(!graph.add_parent(alias::invalid_type, top) && graph.parents(top).empty());
}

void ranges() {
    using alias::AliasResult;
    using alias::ByteRange;
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    constexpr auto largest = std::numeric_limits<std::uint64_t>::max();
    const auto maximal = alias::checked_range(low, largest);
    assert(maximal && maximal->begin == low && maximal->end == high);
    assert(!alias::checked_range(low + 1, largest));
    assert(!alias::checked_range(high, 1));
    assert(alias::checked_offset_add(low, high) == -1);
    assert(!alias::checked_offset_add(high, 1) && !alias::checked_offset_add(low, -1));
    assert(alias::checked_range(-3, 2)->end == -1);
    assert(alias::checked_range(-3, 3)->end == 0);
    assert(alias::range_alias({2, 1}, {5, 6}) == AliasResult::MayAlias);
    assert(!(ByteRange{0, 8}.contains({5, 3})));
    assert((ByteRange{0, 8}.contains({8, 8})));

    for (int a = -4; a <= 4; ++a) for (std::uint64_t as = 0; as <= 4; ++as)
        for (int b = -4; b <= 4; ++b) for (std::uint64_t bs = 0; bs <= 4; ++bs) {
            bool overlaps = false;
            for (int x = a; x < a + static_cast<int>(as); ++x)
                for (int y = b; y < b + static_cast<int>(bs); ++y) overlaps |= x == y;
            const auto expected = overlaps ? AliasResult::MustAlias : AliasResult::NoAlias;
            assert(alias::alias({2, a, as}, {2, b, bs}) == expected);
        }
    assert(alias::alias({1, high, 1}, {1, 0, 1}) == AliasResult::MayAlias);
    assert(alias::alias({1, {}, 1}, {1, 0, 1}) == AliasResult::MayAlias);
    assert(alias::alias({1}, {1, 0, 1}) == AliasResult::MayAlias);
    assert(alias::alias({}, {}) == AliasResult::MayAlias);
    assert(alias::alias({ir::invalid_value, {}, 0}, {}) == AliasResult::NoAlias);
}

void points_to() {
    using alias::AliasResult;
    using ir::Opcode;
    using ir::Operand;

    const ir::Function function{"example", 0, {32}, {
        {0, {
            inst(Opcode::AddressOf, 1, {}, {}, {}, "first"),
            inst(Opcode::AddressOf, 2, {}, {}, {}, "second"),
            inst(Opcode::AddressOf, 3, {}, {}, {}, "first"),
            inst(Opcode::Copy, 4, {Operand::ref(1)}),
            inst(Opcode::Copy, 5, {Operand::ref(1), Operand::constant(2)}),
            inst(Opcode::Load, 6, {Operand::ref(1)}),
            inst(Opcode::Call, 7, {Operand::ref(1)}, {}, {}, "external"),
            inst(Opcode::Add, 8, {Operand::ref(1), Operand::constant(4)}),
            inst(Opcode::Branch, {}, {Operand::ref(32)}, {10, 20}),
        }},
        {10, {inst(Opcode::Jump, {}, {}, {30})}},
        {20, {inst(Opcode::Jump, {}, {}, {30})}},
        {30, {
            inst(Opcode::Phi, 9, {Operand::ref(1), Operand::constant(0), Operand::ref(2)}, {}, {10, 20, 40}),
            inst(Opcode::Phi, 10, {Operand::ref(1), Operand::ref(1), Operand::constant(0)}, {}, {10, 20, 40}),
            inst(Opcode::Return),
        }},
        {40, {inst(Opcode::Jump, {}, {}, {30})}},
    }};
    const auto points = alias::analyze_points_to(function);
    assert(points.converged);
    assert(alias::alias({1}, {2}, points) == AliasResult::NoAlias);
    assert(alias::alias({1, 0, 4}, {3, 0, 4}, points) == AliasResult::MayAlias);
    assert(alias::alias({4}, {2}, points) == AliasResult::NoAlias);
    assert(alias::alias({10}, {2}, points) == AliasResult::NoAlias); // dead edge
    for (ir::ValueId id : {5u, 6u, 7u, 8u, 9u, 32u, 90u})
        assert(alias::alias({id}, {2}, points) == AliasResult::MayAlias);
    const auto short_run = alias::analyze_points_to(function, 1);
    assert(!short_run.converged && alias::alias({1}, {2}, short_run) == AliasResult::MayAlias);
    assert(!alias::analyze_points_to({}).converged);
    auto opaque_edge = function;
    opaque_edge.block(0).instructions[0].targets = {40};
    assert(!alias::analyze_points_to(opaque_edge).converged);
}

void access_and_calls() {
    using alias::AccessKind;
    using ir::Opcode;
    using ir::Operand;

    const auto load = alias::memory_access(inst(Opcode::Load, 2, {Operand::ref(1)}), 4, 5);
    assert(load && load->block == 4 && load->instruction == 5 && load->kind == AccessKind::Read);
    assert(load->location.pointer == 1 && !load->location.size);
    const auto store = alias::memory_access(
        inst(Opcode::Store, {}, {Operand::constant(2), Operand::ref(1)}), 4, 6, 2);
    assert(store && store->kind == AccessKind::Write && store->location.pointer == ir::invalid_value);
    assert(store->location.size == 2);
    const auto malformed = alias::memory_access(inst(Opcode::Store), 4, 7, 0);
    assert(malformed && malformed->kind == AccessKind::Unknown && !malformed->location.size);
    assert(!alias::memory_access(inst(Opcode::Call), 4, 8));

    const ir::Function function{"example", 4, {}, {{4, {
        inst(Opcode::Call, 2, {Operand::constant(7), Operand::ref(1)}, {}, {}, "reader"),
        inst(Opcode::Call, {}, {Operand::ref(1)}),
        inst(Opcode::Call, {}, {}, {}, {}, "unresolved"),
        inst(Opcode::Call, {}, {ir::Operand{}}, {}, {}, "reader"),
    }}}};
    const auto sites = alias::call_sites(function);
    assert(sites.size() == 4);
    assert(sites[0].is_direct() && sites[0].block == 4 && sites[0].instruction == 0);
    assert(sites[0].result == 2 && sites[0].operands[0] == Operand::constant(7));
    assert(!sites[1].is_direct() && !sites[3].is_direct());
    const alias::CallSummaries summaries{{"reader", {.reads = true}},
                                        {"", {}}, {"unresolved", {.unknown_call = true}}};
    assert(alias::call_effects(sites[0], summaries).reads);
    assert(!alias::call_effects(sites[0], summaries).writes);
    for (const auto index : {1u, 2u, 3u}) {
        const auto effects = alias::call_effects(sites[index], summaries);
        assert(effects.reads && effects.writes && effects.allocates && effects.frees && effects.unknown_call);
    }
    assert(alias::call_effects(sites[0]).unknown_call);
}

} // namespace

int main() {
    types();
    ranges();
    points_to();
    access_and_calls();
}
