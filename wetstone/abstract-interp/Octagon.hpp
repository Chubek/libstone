#pragma once
#include "DBM.hpp"
namespace wetstone::ai {
// Encodes ±x±y<=c with two signed nodes per variable.
class Octagon {
    std::size_t vars_=0;DBM dbm_;
    static std::size_t sign_index(std::size_t v,int sign){if(sign!=1&&sign!=-1)throw std::invalid_argument("sign");return 2*v+(sign==-1);}
public:
    explicit Octagon(std::size_t n):vars_(n),dbm_(2*n){}
    static Octagon top(std::size_t n){return Octagon(n);}
    static Octagon bottom(std::size_t n){Octagon x(n);x.dbm_=DBM::bottom(2*n);return x;}
    bool is_bottom()const{return dbm_.is_bottom();}
    void constrain(std::size_t x,int sign_x,std::size_t y,int sign_y,std::int64_t c){
        if(x>=vars_||y>=vars_)throw std::out_of_range("octagon var");
        dbm_.constrain(sign_index(x,sign_x),sign_index(y,-sign_y),c);
        dbm_.constrain(sign_index(y,sign_y),sign_index(x,-sign_x),c);
    }
    // Unary bounds use doubled difference representation.
    void upper(std::size_t x,std::int64_t c){if(c>DBM::inf/2) return;constrain(x,1,x,1,2*c);}
    void lower(std::size_t x,std::int64_t c){if(c < -DBM::inf/2)return;constrain(x,-1,x,-1,-2*c);}
    bool leq(const Octagon& o)const{return dbm_.leq(o.dbm_);}
    Octagon join(const Octagon& o)const{Octagon r=*this;r.dbm_=dbm_.join(o.dbm_);return r;}
    Octagon meet(const Octagon& o)const{Octagon r=*this;r.dbm_=dbm_.meet(o.dbm_);return r;}
    Octagon widen(const Octagon& o)const{Octagon r=*this;r.dbm_=dbm_.widen(o.dbm_);return r;}
    friend bool operator==(const Octagon& a,const Octagon& b){return a.vars_==b.vars_&&a.dbm_==b.dbm_;}
};
}
