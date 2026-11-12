#include <supersonic/Supersonic.hpp>
#include <iostream>
int main(){using namespace moonstone::supersonic;Problem p;p.grammar.variables={"x","y"};p.grammar.constants={0,1};p.examples={{{{"x",1},{"y",2}},3},{{{"x",3},{"y",4}},7}};auto solution=solve(p);if(solution.program)std::cout<<solution.program->print()<<'\n';return solution.program?0:1;}
