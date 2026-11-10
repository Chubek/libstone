#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;

// ---- BitvectorInterval: exhaustive small-range soundness ----
static void test_bitvector_soundness() {
    using B = ai::BitvectorInterval;
    const unsigned w = 8;
    const std::uint64_t m = B::mask(w);
    for (std::uint64_t a = 0; a <= m; ++a)
        for (std::uint64_t b = 0; b <= m; ++b) {
            auto pa = B::point(a, w), pb = B::point(b, w);
            assert(pa.add(pb).contains((a + b) & m));
            assert(pa.sub(pb).contains((a - b) & m));
            assert(pa.mul(pb).contains((a * b) & m));
            if (b) { assert(pa.div(pb).contains(a / b)); assert(pa.modulo(pb).contains(a % b)); }
            assert(pa.neg().contains((0 - a) & m));
            assert(pa.bit_and(pb).contains((a & b) & m));
            assert(pa.bit_or(pb).contains((a | b) & m));
            assert(pa.bit_xor(pb).contains((a ^ b) & m));
            assert(pa.bit_not().contains((~a) & m));
            for (std::uint64_t k = 0; k < w; ++k) {
                assert(pa.shl(B::point(k, w)).contains((a << k) & m));
                assert(pa.shr(B::point(k, w)).contains(a >> k));
            }
        }
    // Lattice and wrap-to-top behaviour.
    assert(B::point(200, w).add(B::point(100, w)).is_top());      // 300 wraps -> top
    assert(B::point(5, w).sub(B::point(6, w)).is_top());          // underflow -> top
    assert(B::point(20, w).mul(B::point(20, w)).is_top());        // 400 wraps -> top
    assert(B::point(0, w).neg() == B::point(0, w));
    assert(B::point(5, w).neg() == B::point((0 - 5) & m, w));
    // Comparisons on points are exact.
    assert(B::point(3, w).less(B::point(5, w)) == B::point(1, 1));
    assert(B::point(5, w).less(B::point(3, w)) == B::point(0, 1));
    assert(B::point(3, w).equal(B::point(3, w)) == B::point(1, 1));
    assert(B::point(3, w).not_equal(B::point(4, w)) == B::point(1, 1));
}

// ---- TypedInt reduced product ----
static void test_typed_int() {
    using namespace ai;
    static_assert(AbstractDomain<TypedInt>);
    assert(TypedInt::point(5, 8).add(TypedInt::point(3, 8)) == TypedInt::point(8, 8));
    assert(TypedInt::point(0xF0, 8).bit_and(TypedInt::point(0x0F, 8)) == TypedInt::point(0, 8));
    assert(TypedInt::point(0xF0, 8).bit_or(TypedInt::point(0x0F, 8)) == TypedInt::point(0xFF, 8));
    // Contradiction between range and bits collapses to bottom.
    assert(TypedInt::point(5, 8).meet(TypedInt::point(6, 8)).is_bottom());
    // Fully-known bits collapse the range to a singleton via reduce().
    assert(TypedInt::from_bits(KnownBits::constant(42, 8)) == TypedInt::point(42, 8));
    // join is broader than either operand; operands are below the join.
    auto j = TypedInt::point(1, 8).join(TypedInt::point(2, 8));
    assert(TypedInt::point(1, 8).leq(j) && !j.leq(TypedInt::point(1, 8)));
    // Arithmetic that overflows the width degenerates the range to top.
    assert(TypedInt::point(200, 8).add(TypedInt::point(100, 8)).range.is_top());
}

// ---- Octagon strong closure ----
static void test_octagon_strong_closure() {
    ai::Octagon oct(2);
    // x0 - x1 <= 4   and   x0 + x1 <= 7   imply   2*x0 <= 11 by plain closure,
    // but since 2*x0 is always even the tight bound is 2*x0 <= 10 (x0 <= 5).
    // Strong closure is what recovers the even bound.
    oct.constrain(0, +1, 1, -1, 4);   // x0 - x1 <= 4
    oct.constrain(0, +1, 1, +1, 7);   // x0 + x1 <= 7
    assert(!oct.is_bottom());
    assert(oct.bound(0, +1, 0, +1) == 10);   // 2*x0 <= 10
    // Consistency check: upper/lower that contradict must still be bottom.
    ai::Octagon bad(2); bad.upper(0, 4); bad.lower(0, 7);
    assert(bad.is_bottom());
}

// ---- Narrowing operator and solver integration ----
static void test_narrowing() {
    using namespace ai;
    // Widening retracted: a [+inf] endpoint is pulled back to the successor's
    // finite bound, never dropping below the successor.
    Environment widened; widened.assign(7, {0, INT64_MAX});
    Environment succ;    succ.assign(7, {0, 50});
    auto n = widened.narrow(succ);
    assert(n.get(7) == Interval(0, 50));
    assert(n.get(7).leq(widened.get(7)));   // refined (tighter)
    assert(succ.get(7).leq(n.get(7)));      // never below the joined successor
    // A point operand leaves no infinite endpoint to retract.
    Environment p; p.assign(1, {3, 3});
    assert(p.narrow(p).get(1) == Interval(3, 3));

    // Solver integration: narrowing is monotone, so every narrowed fact must
    // remain below (more precise than or equal to) the widened post-fixpoint.
    ir::Function f; f.name = "loop"; f.entry = 0;
    f.blocks = {
        {0, {{ir::Opcode::Constant, 1, {ir::Operand::constant(0)}},
             {ir::Opcode::Jump, {}, {}, {1}}}},
        {1, {{ir::Opcode::Phi, 2, {ir::Operand::ref(1), ir::Operand::ref(3)}, {}, {0, 1}},
             {ir::Opcode::Constant, 4, {ir::Operand::constant(1)}},
             {ir::Opcode::Add, 3, {ir::Operand::ref(2), ir::Operand::ref(4)}},
             {ir::Opcode::Jump, {}, {}, {1}}}}
    };
    f.validate();
    auto widened_run = ai::analyze_intervals(f, 1000, 1, 0);
    auto narrowed_run = ai::analyze_intervals(f, 1000, 1, 5);
    assert(widened_run.converged && narrowed_run.converged);
    for (const auto& [id, wo] : narrowed_run.out)
        assert(wo.leq(widened_run.out.at(id)));
    for (const auto& [id, wi] : narrowed_run.in)
        assert(wi.leq(widened_run.in.at(id)));
}

int main() {
    test_bitvector_soundness();
    test_typed_int();
    test_octagon_strong_closure();
    test_narrowing();
    std::cout << "Typed/bitvector/octagon/narrowing tests passed\n";
}
