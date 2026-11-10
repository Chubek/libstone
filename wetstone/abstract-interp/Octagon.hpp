#pragma once
#include "DBM.hpp"
namespace wetstone::ai {
// Encodes +/-x_i +/-x_j <= c with two signed nodes per variable (node 2i = +x_i,
// node 2i+1 = -x_i) on top of a difference-bound matrix.
class Octagon {
    std::size_t vars_=0;DBM dbm_;
    static std::size_t sign_index(std::size_t v,int sign){if(sign!=1&&sign!=-1)throw std::invalid_argument("sign");return 2*v+(sign==-1);}
    // Octagon strong closure (Mine, "The Octagon Abstract Domain"). After the
    // ordinary difference-bound Floyd-Warshall closure, the unary constraints
    // 2*x_i <= c and -2*x_i <= c can be tightened to the nearest even bound,
    // because 2*x_i is always even. Re-closing then propagates those tightened
    // unary facts into binary constraints; iterate to a fixpoint. This only ever
    // tightens, so it preserves soundness while recovering precision that the
    // plain DBM closure misses. Widening deliberately skips it so convergence is
    // not disturbed.
    void strong_close(){
        if(dbm_.is_bottom())return;
        bool changed=true;
        while(changed){
            changed=false;
            for(std::size_t i=0;i<vars_;++i){
                auto& up=dbm_.ref(2*i,2*i+1);   // 2*x_i <= up
                if(up!=DBM::inf&&(up&1)){--up;changed=true;}
                auto& dn=dbm_.ref(2*i+1,2*i);   // -2*x_i <= dn
                if(dn!=DBM::inf&&(dn&1)){--dn;changed=true;}
            }
            dbm_.close();
            if(dbm_.is_bottom())return;
        }
    }
public:
    explicit Octagon(std::size_t n):vars_(n),dbm_(2*n){}
    static Octagon top(std::size_t n){return Octagon(n);}
    static Octagon bottom(std::size_t n){Octagon x(n);x.dbm_=DBM::bottom(2*n);return x;}
    std::size_t variables()const{return vars_;}
    bool is_bottom()const{return dbm_.is_bottom();}
    void constrain(std::size_t x,int sign_x,std::size_t y,int sign_y,std::int64_t c){
        if(x>=vars_||y>=vars_)throw std::out_of_range("octagon var");
        dbm_.constrain(sign_index(x,sign_x),sign_index(y,-sign_y),c);
        dbm_.constrain(sign_index(y,sign_y),sign_index(x,-sign_x),c);
        strong_close();
    }
    // Unary bounds use doubled difference representation.
    void upper(std::size_t x,std::int64_t c){if(c>DBM::inf/2) return;constrain(x,1,x,1,2*c);}
    void lower(std::size_t x,std::int64_t c){if(c < -DBM::inf/2)return;constrain(x,-1,x,-1,-2*c);}
    bool leq(const Octagon& o)const{return dbm_.leq(o.dbm_);}
    Octagon join(const Octagon& o)const{Octagon r=*this;r.dbm_=dbm_.join(o.dbm_);r.strong_close();return r;}
    Octagon meet(const Octagon& o)const{Octagon r=*this;r.dbm_=dbm_.meet(o.dbm_);r.strong_close();return r;}
    Octagon widen(const Octagon& o)const{Octagon r=*this;r.dbm_=dbm_.widen(o.dbm_);return r;}
    // Tight closed bound on sign_x*x + sign_y*y (returns DBM::inf if unbounded).
    std::int64_t bound(std::size_t x,int sign_x,std::size_t y,int sign_y)const{
        if(x>=vars_||y>=vars_)throw std::out_of_range("octagon var");
        return dbm_.bound(sign_index(x,sign_x),sign_index(y,-sign_y));
    }
    friend bool operator==(const Octagon& a,const Octagon& b){return a.vars_==b.vars_&&a.dbm_==b.dbm_;}
};
} // namespace wetstone::ai
