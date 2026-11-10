#pragma once
#include "Ctrlflow.hpp"
namespace wetstone::path {
using DominatorSets=graph::Dominators;
inline DominatorSets dominators(const ir::Function& f){return graph::dominators(f);}
inline std::map<ir::BlockId,std::optional<ir::BlockId>> idom(const ir::Function& f){return graph::immediate_dominators(f);}
inline std::map<ir::BlockId,std::set<ir::BlockId>> dominance_frontiers(const ir::Function& f){return graph::dominance_frontiers(f);}
}
