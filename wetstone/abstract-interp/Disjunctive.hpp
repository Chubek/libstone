#pragma once
#include "AbstractDomain.hpp"
#include <algorithm>
#include <vector>
namespace wetstone::ai {
template<AbstractDomain T,std::size_t Capacity=8>class Disjunctive {
    std::vector<T> elements_;
    void normalize(){
        for(std::size_t i=0;i<elements_.size();) {
            bool redundant=false;
            for(std::size_t j=0;j<elements_.size();++j)if(i!=j&&elements_[i].leq(elements_[j]) &&
                (j<i||!elements_[j].leq(elements_[i]))){redundant=true;break;}
            if(redundant)elements_.erase(elements_.begin()+i);else ++i;
        }
        if(elements_.size()>Capacity){T united=T::bottom();for(const auto& e:elements_)united=united.join(e);
            elements_={std::move(united)};}
    }
public:
    static Disjunctive top(){Disjunctive r;r.elements_.push_back(T::top());return r;}
    static Disjunctive bottom(){return {};}
    static Disjunctive singleton(T x){Disjunctive r;r.elements_.push_back(std::move(x));r.normalize();return r;}
    const std::vector<T>& elements()const{return elements_;}
    bool leq(const Disjunctive& b)const{for(const auto& e:elements_){bool covered=false;
        for(const auto& v:b.elements_)if(e.leq(v)){covered=true;break;}
        if(!covered)return false;}return true;}
    Disjunctive join(const Disjunctive& b)const{Disjunctive r=*this;
        r.elements_.insert(r.elements_.end(),b.elements_.begin(),b.elements_.end());r.normalize();return r;}
    Disjunctive meet(const Disjunctive& b)const{Disjunctive r;
        for(const auto& e:elements_)for(const auto& v:b.elements_){if(r.elements_.size()>=Capacity){r.normalize();}
            r.elements_.push_back(e.meet(v));}r.normalize();return r;}
    Disjunctive widen(const Disjunctive& b)const{
        T all=T::bottom();for(const auto& x:elements_)all=all.join(x);
        for(const auto& x:b.elements_)all=all.join(x);
        if(elements_.empty())return b;if(b.elements_.empty())return *this;
        T old=T::bottom();for(const auto& x:elements_)old=old.join(x);
        return singleton(old.widen(all));
    }
    friend bool operator==(const Disjunctive& a,const Disjunctive& b){return a.leq(b)&&b.leq(a);}
};
}
