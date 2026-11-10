#pragma once
#include <set>
namespace wetstone::df {
template<class T>struct MaySet {
    std::set<T> values;
    bool all=false;
    static MaySet top(){MaySet r;r.all=true;return r;}
    static MaySet bottom(){return {};}
    bool leq(const MaySet& b)const{if(all)return b.all;if(b.all)return true;for(const auto& x:values)if(!b.values.count(x))return false;return true;}
    MaySet join(const MaySet& b)const{if(all||b.all)return top();auto r=*this;r.values.insert(b.values.begin(),b.values.end());return r;}
    MaySet meet(const MaySet& b)const{if(all)return b;if(b.all)return *this;MaySet r;for(const auto& x:values)if(b.values.count(x))r.values.insert(x);return r;}
    MaySet widen(const MaySet& b)const{return join(b);}
    friend bool operator==(const MaySet&,const MaySet&)=default;
};
}
