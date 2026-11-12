#include "../ir/PlaidIR.hpp"
#include <cassert>
using namespace moonstone::plaid::ir;
int main(){
    Program ok{{{Op::Capture,{}, {1},0}, {Op::Resume,{1},{},0}}};
    assert(validate(ok).valid);
    Program bad{{{Op::Resume,{55},{},0}}};
    assert(!validate(bad).valid);
    assert(dialect(Op::ParallelFor)==Dialect::Parallel);
}
