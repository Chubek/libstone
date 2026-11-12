#include <supersonic/Supersonic.hpp>
#include <cassert>
using namespace moonstone::supersonic;
int main(){Problem p;p.grammar.variables={"x","y"};p.grammar.constants={0,1};p.examples={{{{"x",2},{"y",3}},5},{{{"x",9},{"y",4}},13},{{{"x",10},{"y",10}},20}};auto r=solve(p);assert(r.status==SolveStatus::Found&&r.program.has_value());for(auto& ex:p.examples)assert(evaluate(*r.program,ex.inputs)==ex.expected);
 p.budget.max_candidates=1;auto s=solve(p);assert(s.status==SolveStatus::BudgetExhausted);}
