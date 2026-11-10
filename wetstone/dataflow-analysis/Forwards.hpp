#pragma once
#include "DataFlowLattice.hpp"
namespace wetstone::df {
template<ai::AbstractDomain State,class Transfer>
Result<State> forwards(const ir::Function& f,Transfer transfer,State initial=State::top(),std::size_t limit=100000) {
    return solve<State>(f,transfer,{{f.entry,initial}},Direction::Forward,limit);
}
}
