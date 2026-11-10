#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;
static ir::Function control(){
    ir::Function f;f.name="control";f.entry=0;f.parameters={1};
    f.blocks={
        {0,{{ir::Opcode::Branch,{}, {ir::Operand::ref(1)}, {1,2}}}},
        {1,{{ir::Opcode::Jump,{}, {}, {0}}}},
        {2,{{ir::Opcode::Return,{}, {ir::Operand::constant(42)}}}}
    };return f;
}
static ir::Function diamond(){
    ir::Function f;f.name="diamond";f.entry=0;f.parameters={1};
    f.blocks={
      {0,{{ir::Opcode::Branch,{}, {ir::Operand::ref(1)}, {1,2}}}},
      {1,{{ir::Opcode::Constant,2,{ir::Operand::constant(10)}},{ir::Opcode::Jump,{}, {}, {3}}}},
      {2,{{ir::Opcode::Constant,3,{ir::Operand::constant(20)}},{ir::Opcode::Jump,{}, {}, {3}}}},
      {3,{{ir::Opcode::Phi,4,{ir::Operand::ref(2),ir::Operand::ref(3)}, {}, {1,2}},
          {ir::Opcode::Return,{}, {ir::Operand::ref(4)}}}}
    };return f;
}
int main(){
    auto f=control();f.validate();ir::verify_ssa(f);
    assert(path::can_reach(f,1,2));assert(!path::can_reach(f,2,1));
    auto dom=path::dominators(f);assert(dom.at(1).count(0));
    auto loops=path::natural_loops(f);assert(loops.size()==1);assert(loops[0].header==0);
    assert(loops[0].body.count(0)&&loops[0].body.count(1));
    auto components=graph::strongly_connected_components(f);assert(components.size()==2);
    auto paths=path::enumerate(f,10,6,2);assert(!paths.paths.empty());assert(paths.truncated);
    auto flow=ai::analyze_intervals(f,500);assert(flow.converged);
    auto live=df::liveness(f);assert(live.in.at(0).values.count(1));
    auto d=diamond();d.validate();ir::verify_ssa(d);
    auto pd=path::postdominators(d);assert(pd.at(0).count(3));
    auto fronts=path::dominance_frontiers(d);assert(fronts.at(1).count(3));
    auto slice=df::backward_slice(d,4);assert(slice.values.count(2)&&slice.values.count(3));
    auto result=symbolic::execute(d,nullptr,50);assert(result.completed.size()==2);
    auto interval=ai::analyze_intervals(d,200);assert(interval.out.at(3).get(4).contains(10));
    assert(interval.out.at(3).get(4).contains(20));
    std::cout<<"CFG tests passed\n";
}
