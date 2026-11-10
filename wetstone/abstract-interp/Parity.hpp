#pragma once
#include <cstdint>
namespace wetstone::ai {
struct Parity {
    unsigned mask=3; // bit 1 even, bit 2 odd
    static Parity top(){return {3};}static Parity bottom(){return {0};}
    static Parity even(){return {1};}static Parity odd(){return {2};}
    static Parity point(std::int64_t n){return n%2?odd():even();}
    bool leq(Parity o)const{return (mask&~o.mask)==0;}
    Parity join(Parity o)const{return {mask|o.mask};}
    Parity meet(Parity o)const{return {mask&o.mask};}
    Parity widen(Parity o)const{return join(o);}
    Parity add(Parity o)const{unsigned m=0;for(int a=0;a<2;++a)for(int b=0;b<2;++b)
        if((mask&(1u<<a))&&(o.mask&(1u<<b)))m|=1u<<((a+b)&1);return {m};}
    Parity mul(Parity o)const{unsigned m=0;for(int a=0;a<2;++a)for(int b=0;b<2;++b)
        if((mask&(1u<<a))&&(o.mask&(1u<<b)))m|=1u<<(a*b);return {m};}
    friend bool operator==(Parity,Parity)=default;
};
}
