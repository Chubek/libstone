#include "cephyr/mir_schedule.hpp"

#include <cassert>

using namespace cephyr::mir;

int main()
{
    static_assert(lookup_opcode("call_indirect")->fixed_inputs == 1);
    static_assert(lookup_opcode("call_indirect")->accepts_inputs(20));
    static_assert(!lookup_opcode("call_indirect")->accepts_inputs(0));
    static_assert(lookup_opcode("and")->opcode == Opcode::and_);
    static_assert(lookup_opcode("nonsense") == nullptr);
    static_assert(opcode_info(Opcode::invalid) == nullptr);
    static_assert(lookup_value_type("f80") == ValueType::f80);

    Graph g;
    const auto c = [](NodeId source) { return Input{source, EdgeKind::control}; };
    const auto d = [](NodeId source) { return Input{source, EdgeKind::data}; };
    const auto e = [](NodeId source) { return Input{source, EdgeKind::effect}; };
    const auto make = [&](NodeKind kind, BlockId block, Opcode opcode = Opcode::invalid,
                          ValueId value = no_value, ValueType type = ValueType::void_) {
        Node node;
        node.kind = kind;
        node.block = block;
        node.opcode = opcode;
        node.value = value;
        node.type = type;
        return g.add_node(std::move(node));
    };
    const auto edges = [&](NodeId id, std::initializer_list<Input> inputs) {
        g.nodes[id].inputs = g.add_inputs(inputs);
    };

    g.start = make(NodeKind::start, no_block);
    const auto ptr = make(NodeKind::parameter, no_block, Opcode::invalid, 0, ValueType::ptr);
    const auto cond = make(NodeKind::parameter, no_block, Opcode::invalid, 1, ValueType::i64);
    edges(ptr, {c(g.start)});
    edges(cond, {c(g.start)});
    for (BlockId b = 0; b < 4; ++b) {
        const auto region = make(NodeKind::region, b);
        const auto term = make(NodeKind::terminator, b,
            b == 1 ? Opcode::br : (b == 3 ? Opcode::ret : Opcode::cbr));
        g.blocks.push_back({region, term, {}});
    }
    const auto [r0, t0, _0] = g.blocks[0];
    const auto [r1, t1, _1] = g.blocks[1];
    const auto [r2, t2, _2] = g.blocks[2];
    const auto [r3, t3, _3] = g.blocks[3];

    // Insert a use before its definition to exercise real graph scheduling.
    const auto st = make(NodeKind::operation, 0, Opcode::store);
    const auto imm = make(NodeKind::operation, 0, Opcode::const_, 2, ValueType::i64);
    const auto ld = make(NodeKind::operation, 0, Opcode::load, 3, ValueType::i64);
    g.nodes[imm].immediate = UINT64_MAX;
    edges(st, {c(r0), e(r0), d(ptr), d(imm)});
    edges(imm, {c(r0)});
    edges(ld, {e(st), d(ptr), c(r0)});

    const auto local = make(NodeKind::operation, 1, Opcode::copy, 4, ValueType::i64);
    edges(local, {d(ld), c(r1)});
    const auto phi = make(NodeKind::parameter, 2, Opcode::invalid, 5, ValueType::i64);
    const auto next = make(NodeKind::operation, 2, Opcode::add, 6, ValueType::i64);
    edges(phi, {c(r2)});
    edges(next, {d(phi), d(imm), c(r2)});
    g.blocks[2].parameters = g.add_inputs({d(phi)});

    g.successors = {{t0, 1, {}}, {t0, 2, g.add_inputs({d(ld)})},
                    {t1, 2, g.add_inputs({d(local)})},
                    {t2, 2, g.add_inputs({d(next)})}, {t2, 3, {}}};
    edges(t0, {c(r0), e(ld), d(cond)});
    edges(t1, {c(r1), e(r1)});
    edges(t2, {c(r2), e(r2), d(cond)});
    edges(t3, {c(r3), e(r3), d(next)});
    edges(r0, {c(g.start), e(g.start)});
    edges(r1, {c(t0), e(t0)});
    edges(r2, {c(t0), e(t0), c(t1), e(t1), c(t2), e(t2)});
    edges(r3, {c(t2), e(t2)});

    const auto linear = schedule(g);
    assert(linear && (*linear)[0] == (std::vector<NodeId>{imm, st, ld}));
    assert((*linear)[1] == std::vector<NodeId>{local});
    assert((*linear)[2] == std::vector<NodeId>{next});
    assert((*linear)[3].empty());

    Graph bad = g;
    bad.nodes[next].inputs = bad.add_inputs({c(r2), d(local), d(imm)});
    assert(!schedule(bad)); // b1 does not dominate b2.
    bad = g;
    bad.nodes[ld].inputs = bad.add_inputs({c(r0), e(r0), d(ptr)});
    assert(!schedule(bad)); // effect chain forks.
    bad = g;
    bad.nodes[next].inputs = bad.add_inputs({c(r2), d(next), d(imm)});
    assert(!schedule(bad)); // same-block data cycle, unlike a CFG loop.
    bad = g;
    bad.successors[3].arguments = bad.add_inputs({d(ptr)});
    assert(!schedule(bad)); // incoming argument type does not match the phi.
    bad = g;
    bad.nodes[t2].opcode = Opcode::invalid;
    assert(!schedule(bad)); // unresolved operation cannot be emitted.
    bad = g;
    bad.nodes[t2].inputs = {SIZE_MAX, 1};
    assert(!schedule(bad));

    // A conditional may send two different values to the same block.
    auto repeated = g;
    repeated.nodes[t1].opcode = Opcode::cbr;
    repeated.nodes[t1].inputs = repeated.add_inputs({c(r1), e(r1), d(cond)});
    repeated.successors.insert(repeated.successors.begin() + 3,
        {t1, 2, repeated.add_inputs({d(imm)})});
    repeated.nodes[r2].inputs = repeated.add_inputs({c(t0), e(t0), c(t1), e(t1),
                                                     c(t1), e(t1), c(t2), e(t2)});
    assert(schedule(repeated) == linear);

    // The native path need not fit the acsem AST or interpreter depth cap.
    Graph long_chain;
    long_chain.start = long_chain.add_node(Node{.kind = NodeKind::start});
    const auto region = long_chain.add_node(Node{
        .kind = NodeKind::region, .block = 0,
        .inputs = long_chain.add_inputs({c(long_chain.start), e(long_chain.start)})});
    auto last = long_chain.add_node(Node{
        .block = 0, .opcode = Opcode::const_, .type = ValueType::i64, .value = 0,
        .inputs = long_chain.add_inputs({c(region)})});
    for (ValueId value = 1; value < 2500; ++value) {
        last = long_chain.add_node(Node{
            .block = 0, .opcode = Opcode::copy, .type = ValueType::i64, .value = value,
            .inputs = long_chain.add_inputs({c(region), d(last)})});
    }
    const auto ret = long_chain.add_node(Node{
        .kind = NodeKind::terminator, .block = 0, .opcode = Opcode::ret,
        .inputs = long_chain.add_inputs({c(region), e(region), d(last)})});
    long_chain.blocks.push_back({region, ret, {}});
    const auto long_schedule = schedule(long_chain);
    assert(long_schedule && long_schedule->front().size() == 2500);
    assert(long_schedule->front().back() == last);
}
