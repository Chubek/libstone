#pragma once
#include "../utils/IR.hpp"
#include <map>
namespace wetstone::pointer {
struct Nullness {
    unsigned mask=3; // 1 null; 2 non-null
    static Nullness top(){return {3};}static Nullness bottom(){return {0};}
    static Nullness is_null(){return {1};}static Nullness non_null(){return {2};}
    bool leq(Nullness b)const{return (mask&~b.mask)==0;}
    Nullness join(Nullness b)const{return {mask|b.mask};}
    Nullness meet(Nullness b)const{return {mask&b.mask};}
    Nullness widen(Nullness b)const{return join(b);}
    friend bool operator==(Nullness,Nullness)=default;
};
inline std::map<ir::ValueId,Nullness> nullness(const ir::Function& f){
    std::map<ir::ValueId,Nullness> r;
    for(const auto& b:f.blocks)for(const auto& i:b.instructions)if(i.result){
        Nullness n=Nullness::top();
        if(i.opcode==ir::Opcode::AddressOf||i.opcode==ir::Opcode::Alloc)n=Nullness::non_null();
        else if(i.opcode==ir::Opcode::Constant&&!i.arguments.empty()&&i.arguments[0].is_constant)
            n=i.arguments[0].literal?Nullness::non_null():Nullness::is_null();
        else if(i.opcode==ir::Opcode::Copy&&!i.arguments.empty()&&!i.arguments[0].is_constant)
            n=r.count(i.arguments[0].value)?r.at(i.arguments[0].value):Nullness::top();
        r[*i.result]=n;
    }return r;
}
}
