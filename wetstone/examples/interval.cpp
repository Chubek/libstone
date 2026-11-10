#include "Wetstone.hpp"
#include <iostream>
int main(){
    using namespace wetstone;
    ir::Function f;f.name="example";f.entry=0;f.parameters={0};
    f.blocks={{0,{{ir::Opcode::Constant,1,{ir::Operand::constant(5)}},
                 {ir::Opcode::Add,2,{ir::Operand::ref(0),ir::Operand::ref(1)}},
                 {ir::Opcode::Return,{}, {ir::Operand::ref(2)}}}}};
    auto result=ai::analyze_intervals(f);
    std::cout<<"converged="<<result.converged<<" value2=["<<result.out.at(0).get(2).lo
             <<","<<result.out.at(0).get(2).hi<<"]\n";
}
