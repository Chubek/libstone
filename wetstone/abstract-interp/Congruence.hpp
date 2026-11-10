#pragma once
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace wetstone::ai {
// m==0 means singleton r; m==1 means all integers; explicit empty flag.
struct Congruence {
    std::int64_t modulus=1,residue=0;bool empty=false;
    static Congruence top(){return {};}
    static Congruence bottom(){Congruence x;x.empty=true;return x;}
    static Congruence constant(std::int64_t n){return {0,n,false};}
    static Congruence mod(std::int64_t m,std::int64_t r){
        if(m<=0)throw std::invalid_argument("modulus must be positive");
        auto x=r%m;return {m,x<0?x+m:x,false};
    }
    bool contains(std::int64_t n)const{
        return !empty&&(modulus==0?n==residue:((static_cast<__int128>(n)-residue)%modulus==0));
    }
    bool leq(const Congruence& b)const{
        if(empty)return true;if(b.empty)return false;if(b.modulus==1)return true;
        if(modulus==0)return b.contains(residue);
        if(b.modulus==0)return false;
        return modulus%b.modulus==0 && (static_cast<__int128>(residue)-b.residue)%b.modulus==0;
    }
    Congruence join(const Congruence& b)const{
        if(empty)return b;if(b.empty)return *this;
        auto delta=static_cast<__int128>(residue)-b.residue;
        std::uint64_t abs_delta=delta<0?static_cast<std::uint64_t>(-delta):static_cast<std::uint64_t>(delta);
        auto g=std::gcd(static_cast<std::uint64_t>(modulus),static_cast<std::uint64_t>(b.modulus));
        g=std::gcd(g,abs_delta);
        if(g==0)return constant(residue);
        if(g>INT64_MAX)return top();
        return mod(static_cast<std::int64_t>(g),residue);
    }
    Congruence meet(const Congruence& b)const{
        if(empty||b.empty)return bottom();
        if(modulus==0)return b.contains(residue)?*this:bottom();
        if(b.modulus==0)return contains(b.residue)?b:bottom();
        // General CRT intersection is not required for sound narrowing: returning either
        // operand as-is would be wrong, so use top if not one of the nested cases.
        if(leq(b))return *this;if(b.leq(*this))return b;
        auto g=std::gcd(modulus,b.modulus);
        if((static_cast<__int128>(residue)-b.residue)%g!=0)return bottom();
        // Generalized Chinese remainder theorem. Use a conservative operand
        // if the exact lcm cannot be represented by int64_t.
        const auto m1=modulus/g,m2=b.modulus/g;
        auto inverse=[](__int128 a,__int128 m){
            __int128 t=0,next=1,r=m,n=a;
            while(n){auto q=r/n;auto nt=t-q*next;t=next;next=nt;auto nr=r-q*n;r=n;n=nr;}
            return (t%m+m)%m;
        };
        const auto inv=inverse(m1,m2);
        __int128 k=((static_cast<__int128>(b.residue)-residue)/g*inv)%m2;
        if(k<0)k+=m2;
        const __int128 lcm=static_cast<__int128>(modulus)*m2;
        if(lcm>INT64_MAX)return modulus>=b.modulus?*this:b;
        __int128 v=(static_cast<__int128>(residue)+static_cast<__int128>(modulus)*k)%lcm;
        if(v<0)v+=lcm;
        return mod(static_cast<std::int64_t>(lcm),static_cast<std::int64_t>(v));
    }
    Congruence widen(const Congruence& b)const{return join(b);}
    Congruence add(const Congruence& b)const{
        if(empty||b.empty)return bottom();
        auto g=std::gcd(modulus,b.modulus);auto v=static_cast<__int128>(residue)+b.residue;
        if(g==0)return v>=INT64_MIN&&v<=INT64_MAX?constant(static_cast<std::int64_t>(v)):top();
        return mod(g,static_cast<std::int64_t>(v%g));
    }
    friend bool operator==(const Congruence&,const Congruence&)=default;
};
}
