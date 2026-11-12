#include "../continuum/CPSMachine.hpp"
#include <cassert>
#include <limits>
using namespace moonstone::plaid::continuum;
int main() {
    Program p;
    p.entry=1;
    p.nodes.emplace(1,Node{1,{{Arithmetic::Constant,1,0,0,20}},Transfer::Yield,2});
    p.nodes.emplace(2,Node{2,{{Arithmetic::Constant,2,0,0,22},{Arithmetic::Add,3,1,2}},Transfer::Return,0,0,3});
    auto frame=initial_frame(p);
    auto first=run(p,frame,10);assert(first.status==StepStatus::Suspended);
    auto replay=frame; // pure frame snapshot can be replayed
    auto second=run(p,frame,10);assert(second.status==StepStatus::Completed && second.returned==42);
    assert(run(p,replay,10).returned==42);
    bool invalid=false;try{(void)run(p,frame,10);}catch(const std::logic_error&){invalid=true;}assert(invalid);
    assert(checked_sub(4,7)==-3);
    invalid=false;try{(void)checked_add(std::numeric_limits<std::int64_t>::max(),1);}catch(const std::overflow_error&){invalid=true;}assert(invalid);
}
