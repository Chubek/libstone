#include "Wetstone.hpp"
#include <iostream>
int main(){
    using namespace wetstone::symbolic;
    auto a=Expr::variable("a",Sort::Boolean);
    SatieSolver solver;
    auto status=solver.check({a,logical_not(a)}).status;
    std::cout<<"SAT check: "<<(status==CheckResult::Unsat?"UNSAT":status==CheckResult::Sat?"SAT":"UNKNOWN (vendor absent or unsupported)")<<"\n";
    auto x=Expr::variable("x");
    std::cout<<smtlib2({Expr::binary(Kind::Le,x,Expr::integer(100))});
}
