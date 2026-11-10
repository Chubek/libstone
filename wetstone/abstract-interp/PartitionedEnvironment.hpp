#pragma once
#include "Environment.hpp"
namespace wetstone::ai {
// Bounded map of trace partitions. An overflow summary preserves discarded paths.
template<class Key,std::size_t Limit=16>struct PartitionedEnvironment {
    std::map<Key,Environment> partitions;
    Environment overflow=Environment::bottom();
    void add(Key key,const Environment& env){
        auto it=partitions.find(key);
        if(it!=partitions.end()){it->second=it->second.join(env);return;}
        if(partitions.size()<Limit)partitions.emplace(std::move(key),env);
        else overflow=overflow.join(env);
    }
    Environment collapse()const{Environment r=overflow;
        for(const auto& [key,e]:partitions){(void)key;r=r.join(e);}return r;}
};
}
