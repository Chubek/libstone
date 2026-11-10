#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>
namespace wetstone::ai {
// Exact affine copy-offset facts: x = y + c (no assumed machine overflow).
struct AffineEqualities {
    struct Fact {std::uint32_t base;std::int64_t offset;friend bool operator==(const Fact&,const Fact&)=default;};
    std::map<std::uint32_t,Fact> facts;
    bool unreachable=false;
    static AffineEqualities top(){return {};}
    static AffineEqualities bottom(){AffineEqualities r;r.unreachable=true;return r;}
    void assign(std::uint32_t x,std::uint32_t y,std::int64_t offset){facts.erase(x);if(x==y && offset!=0){facts.clear();return;}facts[x]={y,offset};}
    void kill(std::uint32_t x){facts.erase(x);for(auto it=facts.begin();it!=facts.end();)if(it->second.base==x)it=facts.erase(it);else ++it;}
    bool leq(const AffineEqualities& b)const{if(unreachable)return true;if(b.unreachable)return false;for(auto [id,f]:b.facts){auto it=facts.find(id);if(it==facts.end()||it->second!=f)return false;}return true;}
    AffineEqualities join(const AffineEqualities& b)const{if(unreachable)return b;if(b.unreachable)return *this;AffineEqualities r;
        for(auto [id,f]:facts){auto it=b.facts.find(id);if(it!=b.facts.end()&&it->second==f)r.facts[id]=f;}return r;}
    AffineEqualities meet(const AffineEqualities& b)const{if(unreachable||b.unreachable)return bottom();AffineEqualities r=*this;
        for(auto [id,f]:b.facts){auto it=r.facts.find(id);if(it!=r.facts.end()&&it->second!=f){
            if(it->second.base==f.base)return bottom();
            continue; // Cannot represent two different bases for one variable.
        }r.facts[id]=f;}return r;}
    AffineEqualities widen(const AffineEqualities& b)const{return join(b);}
    friend bool operator==(const AffineEqualities&,const AffineEqualities&)=default;
};
}
