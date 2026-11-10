#pragma once
#include "Interval.hpp"
#include "../utils/IR.hpp"
#include <map>
namespace wetstone::ai {
class Environment {
    bool unreachable_=false;
    std::map<ir::ValueId,Interval> values_; // missing means top
public:
    static Environment top(){return {};}
    static Environment bottom(){Environment x;x.unreachable_=true;return x;}
    bool is_bottom()const{return unreachable_;}
    const std::map<ir::ValueId,Interval>& bindings()const{return values_;}
    Interval get(ir::ValueId id)const{
        if(unreachable_)return Interval::bottom();auto it=values_.find(id);
        return it==values_.end()?Interval::top():it->second;
    }
    void assign(ir::ValueId id,Interval value){
        if(unreachable_)return;
        if(value.empty){*this=bottom();return;}
        if(value.is_top())values_.erase(id);else values_[id]=value;
    }
    void refine(ir::ValueId id,Interval constraint){assign(id,get(id).meet(constraint));}
    bool leq(const Environment& b)const{
        if(unreachable_)return true;if(b.unreachable_)return false;
        for(const auto& [id,bound]:b.values_)if(!get(id).leq(bound))return false;
        return true;
    }
    Environment join(const Environment& b)const{
        if(unreachable_)return b;if(b.unreachable_)return *this;
        Environment r;
        for(auto [id,v]:values_)if(auto it=b.values_.find(id);it!=b.values_.end())r.assign(id,v.join(it->second));
        return r;
    }
    Environment meet(const Environment& b)const{
        if(unreachable_||b.unreachable_)return bottom();Environment r=*this;
        for(auto [id,v]:b.values_)r.refine(id,v);
        return r;
    }
    Environment widen(const Environment& b)const{
        if(unreachable_)return b;if(b.unreachable_)return *this;Environment r;
        for(auto [id,v]:values_)if(auto it=b.values_.find(id);it!=b.values_.end())r.assign(id,v.widen(it->second));
        return r;
    }
    friend bool operator==(const Environment& a,const Environment& b){return a.unreachable_==b.unreachable_&&(a.unreachable_||a.values_==b.values_);}
};
}
