#pragma once
#include "DataFlowLattice.hpp"
namespace wetstone::df {
template<ai::AbstractDomain State,class Transfer>
Result<State> forwards(const ir::Function& f,Transfer transfer,State initial=State::top(),
    std::size_t limit=100000,std::size_t widening_delay=3,std::size_t narrowing_iterations=0) {
    return solve<State>(f,transfer,{{f.entry,std::move(initial)}},Direction::Forward,limit,widening_delay,narrowing_iterations);
}
} // namespace wetstone::df
