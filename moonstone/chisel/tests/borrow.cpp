#include <chisel/Chisel.hpp>
#include <cassert>
using namespace moonstone::chisel;
int main(){
 Function ok{{{0,{{Op::Create,1,0},{Op::BorrowShared,1,10},{Op::Use,1,0},{Op::EndBorrow,1,10},{Op::Move,1,0}}, {}}},0};
 auto r=analyze(ok);assert(r.converged&&r.diagnostics.empty());
 Function bad{{{0,{{Op::Create,1,0},{Op::BorrowShared,1,10},{Op::BorrowUnique,1,11},{Op::Move,1,0}}, {}}},0};
 auto e=analyze(bad);assert(e.diagnostics.size()==2);
 Function branch{{{0,{{Op::Create,1,0}},{1,2}}, {1,{{Op::Move,1,0}},{3}}, {2,{{Op::Use,1,0}},{3}}, {3,{{Op::Use,1,0}}, {}}},0};
 auto b=analyze(branch);assert(!b.diagnostics.empty());
}
