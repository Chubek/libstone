#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;
int main(){
    using namespace ai;
    auto c=Congruence::mod(6,1).meet(Congruence::mod(4,3));
    assert(c.modulus==12&&c.contains(7)&&c.contains(19)&&!c.contains(1));
    auto d=Congruence::mod(4,0).meet(Congruence::mod(6,3));assert(d.empty);
    auto prod=Product<Interval,Parity>{Interval{2,8},Parity::even()};
    assert(prod.join({Interval{3,12},Parity::odd()}).second==Parity::top());
    auto p=Disjunctive<Interval,3>::singleton(Interval{0,2}).join(Disjunctive<Interval,3>::singleton(Interval{5,9}));
    assert(p.elements().size()==2);
    auto t=widen_with_thresholds({0,5},{-2,10},{-5,0,5,10,20});assert(t==Interval(-5,10));
    PartitionedEnvironment<int,2> partitions;
    Environment e=Environment::top();e.assign(1,{1,2});partitions.add(0,e);
    e.assign(1,{5,6});partitions.add(1,e);
    e.assign(1,{8,9});partitions.add(2,e);
    assert(partitions.collapse().get(1)==Interval(1,9));
    auto ref=refine_compare(Environment::top(),ir::Instruction{ir::Opcode::Lt,3,{ir::Operand::ref(1),ir::Operand::constant(10)}},true);
    assert(ref.get(1).hi==9);
    auto falseBranch=refine_compare(Environment::top(),ir::Instruction{ir::Opcode::Lt,3,{ir::Operand::ref(1),ir::Operand::constant(INT64_MIN)}},true);
    assert(falseBranch.is_bottom());
    std::cout<<"Domain tests passed\n";
}
