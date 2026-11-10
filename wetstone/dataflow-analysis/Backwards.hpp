#pragma once
#include "DataFlowLattice.hpp"
namespace wetstone::df {
template<ai::AbstractDomain State,class Transfer>
Result<State> backwards(const ir::Function& f,Transfer transfer,std::map<ir::BlockId,State> exits={},std::size_t limit=100000) {
    return solve<State>(f,transfer,std::move(exits),Direction::Backward,limit);
}
}
