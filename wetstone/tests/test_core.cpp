#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace wetstone;
static ir::Function simple(){
    ir::Function f;f.name="add";f.entry=0;f.parameters={0};
    ir::Block b;b.id=0;
    b.instructions.push_back({ir::Opcode::Constant,1,{ir::Operand::constant(3)}});
    b.instructions.push_back({ir::Opcode::Add,2,{ir::Operand::ref(0),ir::Operand::ref(1)}});
    b.instructions.push_back({ir::Opcode::Return,{}, {ir::Operand::ref(2)}});
    f.blocks.push_back(b);return f;
}
int main(){
    static_assert(ai::AbstractDomain<ai::Interval>);
    static_assert(ai::AbstractDomain<ai::Parity>);
    static_assert(ai::AbstractDomain<ai::Congruence>);
    static_assert(ai::AbstractDomain<ai::KnownBits>);
    {
        auto a=ai::Interval{-2,7};assert(a.meet({0,3})==ai::Interval(0,3));
        assert(a.add({2,4})==ai::Interval(0,11));
        assert((ai::Interval{1,3}.mul({4,5})==ai::Interval(4,15)));
        assert((ai::Interval{1,3}.div({0,5}).is_top()));
        assert((ai::Interval{INT64_MIN,INT64_MIN}.neg().lo==INT64_MAX));
        assert((ai::Interval{0,9}.widen({-1,10}).is_top()));
        assert(ai::Interval::bottom().leq(a));
        assert((ai::Interval{1,2}.meet({5,6}).empty));
    }
    {
        assert(ai::Parity::odd().add(ai::Parity::odd())==ai::Parity::even());
        assert(ai::Sign::negative().mul(ai::Sign::negative())==ai::Sign::positive());
        assert(ai::Congruence::mod(4,1).join(ai::Congruence::mod(4,3))==ai::Congruence::mod(2,1));
        assert(ai::Congruence::mod(4,1).contains(17));
        auto r=ai::IntervalCongruence{ai::Interval{0,20},ai::Congruence::mod(4,1)};r.reduce();assert(r.interval==ai::Interval(1,17));
        assert(ai::KnownBits::constant(0xF0,8).bit_and(ai::KnownBits::constant(0x0F,8))==ai::KnownBits::constant(0,8));
        auto f=ai::FiniteSet::point(1,2).join(ai::FiniteSet::point(2,2));assert(f.values().size()==2);
        f=f.join(ai::FiniteSet::point(3,2));assert(f.is_top());
    }
    {
        ai::DBM dbm(3);dbm.constrain(1,0,5);dbm.constrain(2,1,3);assert(dbm.bound(2,0)==8);
        dbm.constrain(0,2,-9);assert(dbm.is_bottom());
        ai::Octagon oct(2);oct.upper(0,4);oct.lower(0,7);assert(oct.is_bottom());
    }
    {
        auto f=simple();f.validate();auto a=ai::analyze_intervals(f);assert(a.converged);
        assert(a.out.at(0).get(2).lo==INT64_MIN+3); // arg0 initially top; arithmetic saturates
        auto live=df::liveness(f);assert(live.converged);assert(live.in.at(0).values.count(0));
        auto d=df::definite_assignment(f);assert(d.out.at(0).count(2));
        auto rd=df::reaching_definitions(f);assert(rd.out.at(0).values.count({0,1}));
        auto s=df::sccp(f);assert(s.converged);
        assert(s.values.at(1)==ai::Constant::point(3));
    }
    {
        symbolic::SatieSolver solver;
        auto x=symbolic::Expr::variable("x",symbolic::Sort::Boolean);
        auto cnf=symbolic::BooleanCNF{};
        cnf.assert_true(symbolic::logical_and(x,symbolic::logical_not(x)));
        assert(cnf.supported);
        assert(!symbolic::smtlib2({symbolic::Expr::binary(symbolic::Kind::Lt,
                symbolic::Expr::variable("a"),symbolic::Expr::integer(3))}).empty());
        assert(symbolic::evaluate(symbolic::Expr::binary(symbolic::Kind::Add,
                symbolic::Expr::integer(5),symbolic::Expr::integer(7)))==12);
        assert(solver.check({x}).status==symbolic::CheckResult::Unknown || WETSTONE_HAS_SATIE);
        auto r=symbolic::execute(simple());assert(r.completed.size()==1);assert(r.completed[0].returned.has_value());
    }
    {
        std::vector<pointer::InclusionConstraint> constraints{
            {pointer::InclusionConstraint::Kind::Address,1,0,4},
            {pointer::InclusionConstraint::Kind::Copy,2,1,0},
            {pointer::InclusionConstraint::Kind::Address,3,0,5}};
        auto p=pointer::andersen(constraints);assert(p.converged);assert(p.may_alias(1,2));assert(!p.may_alias(1,3));
        assert(pointer::alias(p,1,2)==pointer::Alias::MustAlias);
    }
    {
        Diagnostics ds{{"WET-TEST","quoted \" test","source.c","f",1,0,Severity::Warning}};
        auto json=sarif::serialize(ds);assert(json.find("\\\" test")!=std::string::npos);
        auto fixes=repair::pattern_based(simple());assert(fixes.empty());
        assert(!repair::report(fixes).size());
    }
    std::cout<<"Wetstone core tests passed\n";
}
