#pragma once
#include "UseDef.hpp"
namespace wetstone::df {
struct Slice {
    std::set<std::pair<ir::BlockId,std::size_t>> instructions;
    std::set<ir::ValueId> values;
};
inline Slice backward_slice(const ir::Function& f,ir::ValueId root){
    auto ud=use_def(f);Slice slice;std::vector<ir::ValueId> work{root};
    while(!work.empty()){
        auto v=work.back();work.pop_back();if(!slice.values.insert(v).second)continue;
        auto def=ud.definitions.find(v);if(def==ud.definitions.end())continue;
        slice.instructions.insert(def->second);
        const auto& [b,n]=def->second;
        for(auto arg:f.block(b).instructions.at(n).arguments)if(!arg.is_constant)work.push_back(arg.value);
    }return slice;
}
}
