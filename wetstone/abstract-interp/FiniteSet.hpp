#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
namespace wetstone::ai {
class FiniteSet {
    std::size_t cap_=32;bool all_=true;std::set<std::int64_t> elements_;
    void compatible(const FiniteSet& b)const{if(cap_!=b.cap_)throw std::invalid_argument("finite set cap mismatch");}
public:
    explicit FiniteSet(std::size_t cap=32):cap_(cap){if(!cap)throw std::invalid_argument("nonzero cap needed");}
    static FiniteSet top(std::size_t cap=32){return FiniteSet(cap);}
    static FiniteSet bottom(std::size_t cap=32){auto r=top(cap);r.all_=false;return r;}
    static FiniteSet point(std::int64_t v,std::size_t cap=32){auto r=bottom(cap);r.elements_.insert(v);return r;}
    bool is_top()const{return all_;}
    bool is_bottom()const{return !all_&&elements_.empty();}
    const std::set<std::int64_t>& values()const{return elements_;}
    bool leq(const FiniteSet& b)const{compatible(b);return b.all_||(!all_&&std::includes(b.elements_.begin(),b.elements_.end(),elements_.begin(),elements_.end()));}
    FiniteSet join(const FiniteSet& b)const{compatible(b);if(all_||b.all_)return top(cap_);auto r=*this;
        r.elements_.insert(b.elements_.begin(),b.elements_.end());if(r.elements_.size()>cap_)return top(cap_);return r;}
    FiniteSet meet(const FiniteSet& b)const{compatible(b);if(all_)return b;if(b.all_)return *this;
        auto r=bottom(cap_);std::set_intersection(elements_.begin(),elements_.end(),b.elements_.begin(),b.elements_.end(),std::inserter(r.elements_,r.elements_.begin()));return r;}
    FiniteSet widen(const FiniteSet& b)const{return join(b);}
    friend bool operator==(const FiniteSet& a,const FiniteSet& b){return a.cap_==b.cap_&&a.all_==b.all_&&(a.all_||a.elements_==b.elements_);}
};
}
