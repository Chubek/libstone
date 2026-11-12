#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
namespace moonstone::chisel {
using Region=std::uint32_t;
// Inclusion-based lifetime constraints: outlives(a,b) asserts a must contain b.
// Closure is transitively complete; clients supply a finite region universe.
class RegionSolver {
 std::map<Region,std::set<Region>> edges_;std::map<Region,std::set<Region>> closure_;
public:
 void declare(Region r){edges_.try_emplace(r);}
 void outlives(Region a,Region b){declare(a);declare(b);edges_[a].insert(b);closure_.clear();}
 void solve(){closure_=edges_;for(auto& [a,bs]:closure_)bs.insert(a);bool changed=true;while(changed){changed=false;for(auto& [a,bs]:closure_){std::set<Region> next=bs;for(auto b:bs){auto it=closure_.find(b);if(it!=closure_.end())next.insert(it->second.begin(),it->second.end());}if(next!=bs){bs=std::move(next);changed=true;}}}}
 bool proves_outlives(Region a,Region b)const{if(closure_.empty())throw std::logic_error("solve() must be called after adding constraints");auto it=closure_.find(a);return it!=closure_.end()&&it->second.count(b);}
 std::vector<std::pair<Region,Region>> constraints()const{std::vector<std::pair<Region,Region>> v;for(auto& [a,bs]:edges_)for(auto b:bs)v.push_back({a,b});return v;}
};
}
