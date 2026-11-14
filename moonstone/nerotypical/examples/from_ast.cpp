// Requires original LibStone Nerotypical Language.hpp and its Satie headers.
// Demonstrates API connection, not an independent typing proof.
#include <nerotypical/sequent/Bridge.hpp>
#include <nerotypical/sequent/SequentIR.hpp>
#include <iostream>
using namespace moonstone::nerotypical;
using namespace moonstone::nerotypical::sequent;
int main() {
  ast::Program source;
  source.body=ast::let("x",ast::int_literal(4),ast::variable("x"));
  // In production, derive these types from a successful original
  // StructuralChecker run.  The oracle here is for a tiny, known program.
  AstBridge bridge([](const ast::Expr&) {return Type::integer();});
  auto ir=bridge.lower_program(source);
  auto checked=Verifier{}.check(ir);
  if(!checked.ok()) {std::cerr<<checked.errors.front()<<'\n';return 1;}
  std::cout<<print_module(ir);
  std::cout<<"result="<<value_string(Evaluator{}.run(optimize_checked(ir)))<<'\n';
}
