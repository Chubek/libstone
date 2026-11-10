#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;
int main(){
    ir::Function f;f.name="pointers";f.entry=0;
    f.blocks={{0,{
      {ir::Opcode::AddressOf,1,{}, {}, {}, "objectA"},
      {ir::Opcode::AddressOf,2,{}, {}, {}, "objectB"},
      {ir::Opcode::Copy,3,{ir::Operand::ref(1)}},
      {ir::Opcode::Store,{}, {ir::Operand::ref(1),ir::Operand::ref(2)}},
      {ir::Opcode::Load,4,{ir::Operand::ref(3)}},
      {ir::Opcode::Call,{}, {ir::Operand::ref(2)}, {}, {}, "external"},
      {ir::Opcode::Free,{}, {ir::Operand::ref(1)}},
      {ir::Opcode::Free,{}, {ir::Operand::ref(1)}},
      {ir::Opcode::Return,{}, {ir::Operand::ref(4)}}}}};
    f.validate();auto a=pointer::PointerAnalyzer::build(f);
    auto r=a.solve();assert(r.converged);
    assert(pointer::alias(r,1,3)==pointer::Alias::MustAlias);
    assert(pointer::alias(r,1,2)==pointer::Alias::NoAlias);
    assert(pointer::alias(r,2,4)==pointer::Alias::MustAlias);
    auto e=pointer::escapes(f,r);assert(e.escaping.count(a.objects.at("objectB")));
    auto warnings=pointer::ownership_warnings(f);assert(warnings.size()==1);
    auto fx=pointer::effects(f);assert(fx.reads&&fx.writes&&fx.frees&&fx.calls);
    assert(pointer::calls(f).size()==1);
    std::cout<<"Pointer tests passed\n";
}
