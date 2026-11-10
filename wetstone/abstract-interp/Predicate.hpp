#pragma once
#include <map>
#include <string>
namespace wetstone::ai {
// Tri-state predicate abstraction; contradictory constraints produce bottom.
struct Predicates {
    std::map<std::string,bool> known;
    bool unreachable=false;
    static Predicates top(){return {};}
    static Predicates bottom(){Predicates r;r.unreachable=true;return r;}
    void assume(const std::string& name,bool value){if(unreachable)return;auto it=known.find(name);if(it!=known.end()&&it->second!=value){*this=bottom();return;}known[name]=value;}
    bool leq(const Predicates& b)const{if(unreachable)return true;if(b.unreachable)return false;for(auto [k,v]:b.known){auto it=known.find(k);if(it==known.end()||it->second!=v)return false;}return true;}
    Predicates join(const Predicates& b)const{if(unreachable)return b;if(b.unreachable)return *this;Predicates r;for(auto [k,v]:known)if(auto i=b.known.find(k);i!=b.known.end()&&i->second==v)r.known[k]=v;return r;}
    Predicates meet(const Predicates& b)const{if(unreachable||b.unreachable)return bottom();Predicates r=*this;for(auto [k,v]:b.known)r.assume(k,v);return r;}
    Predicates widen(const Predicates& b)const{return join(b);}
    friend bool operator==(const Predicates&,const Predicates&)=default;
};
}
