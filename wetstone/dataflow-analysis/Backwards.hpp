#pragma once
#include "DataFlowLattice.hpp"
namespace wetstone::df {
template<ai::AbstractDomain State,class Transfer>
Result<State> backwards(const ir::Function& f,Transfer transfer,std::map<ir::BlockId,State> exits={},
    std::size_t limit=100000,std::size_t widening_delay=3,std::size_t narrowing_iterations=0) {
    return solve<State>(f,transfer,std::move(exits),Direction::Backward,limit,widening_delay,narrowing_iterations);
}
} // namespace wetstone::df
