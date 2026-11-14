#include <nerotypical/sequent/SequentIR.hpp>
#include <iostream>
using namespace moonstone::nerotypical::sequent;
namespace m=moonstone::nerotypical::sequent::make;
int main() {
  Type integer=Type::integer();
  // The body of j shares ret with the enclosing Mu computation.
  auto source=m::mu(integer,m::let_join("j",{{"answer",integer}},
    m::cut(m::variable("answer",integer)),m::jump("j",{m::integer(42)})));
  Kont suffix{{Frame::cases({m::otherwise("v",integer,m::cut(m::boolean(true)))})}};
  Module module;
  module.result_type=Type::boolean();
  module.entry=m::cut(source,suffix);
  std::cout<<"Before:\n"<<print_module(module);
  auto optimized=optimize_checked(module);
  std::cout<<"After:\n"<<print_module(optimized);
  std::cout<<"Result: "<<value_string(Evaluator{}.run(optimized))<<"\n";
  std::cout<<"DOT graph:\n"<<to_dot(optimized);
}
