#pragma once
#include "Interval.hpp"
#include <cstdint>
namespace wetstone::ai {
// Power set over {negative,zero,positive}.
struct Sign {
    unsigned mask=7;
    static Sign top(){return {7};}
    static Sign bottom(){return {0};}
    static Sign negative(){return {1};}
    static Sign zero(){return {2};}
    static Sign positive(){return {4};}
    static Sign from(const Interval& i){if(i.empty)return bottom();return {(i.lo<0?1u:0u)|(i.contains(0)?2u:0u)|(i.hi>0?4u:0u)};}
    bool leq(Sign b)const{return (mask&~b.mask)==0;}
    Sign join(Sign b)const{return {mask|b.mask};}
    Sign meet(Sign b)const{return {mask&b.mask};}
    Sign widen(Sign b)const{return join(b);}
    Sign negate()const{return {(mask&1?4u:0u)|(mask&2?2u:0u)|(mask&4?1u:0u)};}
    Sign add(Sign b)const{
        Sign r=bottom();
        for(unsigned aBit=1;aBit<=4;aBit<<=1)if(mask&aBit)for(unsigned bBit=1;bBit<=4;bBit<<=1)if(b.mask&bBit){
            if(aBit==2)r.mask|=bBit;
            else if(bBit==2)r.mask|=aBit;
            else if(aBit==bBit)r.mask|=aBit;
            else r.mask|=7;
        }return r;
    }
    Sign mul(Sign b)const{
        Sign r=bottom();for(unsigned a=1;a<=4;a<<=1)if(mask&a)for(unsigned c=1;c<=4;c<<=1)if(b.mask&c){
            if(a==2||c==2)r.mask|=2;else r.mask|=(a==c?4u:1u);
        }return r;
    }
    friend bool operator==(Sign,Sign)=default;
};
}
