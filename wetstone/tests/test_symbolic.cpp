#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;
int main(){
    using namespace symbolic;
    auto a=Expr::variable("a",Sort::Boolean);
    auto b=Expr::variable("b",Sort::Boolean);
    auto eq=Expr::binary(Kind::Eq,a,b);
    BooleanCNF encoder;encoder.assert_true(eq);
    assert(encoder.supported&&encoder.clauses.size()==5);
    auto unknown=Expr::binary(Kind::Lt,Expr::variable("i"),Expr::integer(7));
    BooleanCNF theory;theory.assert_true(unknown);assert(!theory.supported);
    assert(evaluate(Expr::binary(Kind::Eq,Expr::integer(5),Expr::integer(5)))==1);
    assert(!evaluate(Expr::binary(Kind::Div,Expr::integer(5),Expr::integer(0))));
    assert(smtlib2({unknown}).find("(check-sat)")!=std::string::npos);
    SatieSolver solver;
    auto report=solver.check({logical_and(a,logical_not(a))});
    assert(report.status==CheckResult::Unsat||report.status==CheckResult::Unknown);
    ir::Function f;f.name="test";f.entry=0;f.parameters={1};
    f.blocks={
       {0,{{ir::Opcode::Eq,2,{ir::Operand::ref(1),ir::Operand::constant(5)}},
           {ir::Opcode::Branch,{}, {ir::Operand::ref(2)}, {1,2}}}},
       {1,{{ir::Opcode::Return,{}, {ir::Operand::constant(1)}}}},
       {2,{{ir::Opcode::Return,{}, {ir::Operand::constant(0)}}}}
    };
    auto run=execute(f,&solver,20);assert(run.completed.size()==2);
    assert(!run.truncated);
    std::cout<<"Symbolic tests passed\n";
}
