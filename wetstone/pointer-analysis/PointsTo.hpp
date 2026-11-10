#pragma once
#include "../utils/IR.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>
namespace wetstone::pointer {
using ObjectId=std::uint32_t;
inline constexpr ObjectId unknown_object=UINT32_MAX;
using PointSet=std::set<ObjectId>;
struct InclusionConstraint {
    enum class Kind{Address,Copy,Load,Store};
    Kind kind=Kind::Copy;ir::ValueId lhs=0,rhs=0;ObjectId object=unknown_object;
};
struct PointsToResult {
    std::map<ir::ValueId,PointSet> variables;
    std::map<ObjectId,PointSet> memory;
    std::size_t iterations=0;bool converged=true;
    PointSet points_to(ir::ValueId id)const{auto i=variables.find(id);return i==variables.end()?PointSet{}:i->second;}
    bool may_alias(ir::ValueId a,ir::ValueId b)const{
        auto x=points_to(a),y=points_to(b);
        if(x.count(unknown_object)||y.count(unknown_object))return true;
        for(auto o:x)if(y.count(o))return true;return false;
    }
};
// Inclusion-based, flow-insensitive Andersen analysis.
inline PointsToResult andersen(const std::vector<InclusionConstraint>& constraints,
                              std::size_t iteration_limit=10000,PointsToResult initial={}){
    PointsToResult r=std::move(initial);bool change=true;
    auto put=[&](PointSet& target,const PointSet& source){auto n=target.size();target.insert(source.begin(),source.end());return n!=target.size();};
    while(change){if(r.iterations++>=iteration_limit){r.converged=false;break;}change=false;
        for(const auto& c:constraints){
            switch(c.kind){
                case InclusionConstraint::Kind::Address:
                    change|=r.variables[c.lhs].insert(c.object).second;break;
                case InclusionConstraint::Kind::Copy:
                    change|=put(r.variables[c.lhs],r.variables[c.rhs]);break;
                case InclusionConstraint::Kind::Load:
                    for(auto object:r.variables[c.rhs]){
                        if(object==unknown_object)change|=r.variables[c.lhs].insert(unknown_object).second;
                        else change|=put(r.variables[c.lhs],r.memory[object]);
                    }break;
                case InclusionConstraint::Kind::Store:
                    for(auto object:r.variables[c.lhs])change|=put(r.memory[object],r.variables[c.rhs]);break;
            }
        }
    }return r;
}
}
