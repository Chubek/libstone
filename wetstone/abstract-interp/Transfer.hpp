#pragma once
#include "Environment.hpp"
namespace wetstone::ai {
inline Interval operand(const Environment& e,const ir::Operand& o){return o.is_constant?Interval::point(o.literal):e.get(o.value);}
inline Interval evaluate(const ir::Instruction& i,const Environment& env){
    using ir::Opcode;const auto& a=i.arguments;
    auto arg=[&](std::size_t n){return n<a.size()?operand(env,a[n]):Interval::top();};
    if(i.opcode==Opcode::Constant||i.opcode==Opcode::Copy||i.opcode==Opcode::Neg)return i.opcode==Opcode::Neg?arg(0).neg():arg(0);
    if(i.opcode==Opcode::Phi){Interval r=Interval::bottom();for(const auto& o:a)r=r.join(operand(env,o));return r;}
    const auto x=arg(0),y=arg(1);
    switch(i.opcode){
        case Opcode::Add:return x.add(y);case Opcode::Sub:return x.sub(y);
        case Opcode::Mul:return x.mul(y);case Opcode::Div:return x.div(y);case Opcode::Mod:return x.modulo(y);
        case Opcode::Eq: if(x.empty||y.empty)return Interval::bottom();if(x.hi<y.lo||y.hi<x.lo)return Interval::point(0);if(x.lo==x.hi&&x==y)return Interval::point(1);return {0,1};
        case Opcode::Ne: {auto eq=evaluate(ir::Instruction{Opcode::Eq,{},a},env);return eq.lo==eq.hi?Interval::point(1-eq.lo):Interval{0,1};}
        case Opcode::Lt:case Opcode::Le:case Opcode::Gt:case Opcode::Ge:{
            if(x.empty||y.empty)return Interval::bottom();
            auto lt=[&](const Interval& u,const Interval& v,bool equal){
                if(equal?(u.hi<=v.lo):(u.hi<v.lo))return Interval::point(1);
                if(equal?(u.lo>v.hi):(u.lo>=v.hi))return Interval::point(0);
                return Interval{0,1};
            };
            return i.opcode==Opcode::Lt?lt(x,y,false):i.opcode==Opcode::Le?lt(x,y,true):
                   i.opcode==Opcode::Gt?lt(y,x,false):lt(y,x,true);
        }
        default:return Interval::top();
    }
}
inline void transfer(Environment& env,const ir::Instruction& i){
    if(env.is_bottom())return;
    if(i.opcode==ir::Opcode::Assume||i.opcode==ir::Opcode::Assert){
        if(!i.arguments.empty()){
            auto a=operand(env,i.arguments[0]);
            if(a.lo==0&&a.hi==0){env=Environment::bottom();return;}
            if(!i.arguments[0].is_constant && i.opcode==ir::Opcode::Assume && a.contains(0)){
                // General nonzero is nonconvex: narrow only when interval endpoint is zero.
                if(a.lo==0)env.refine(i.arguments[0].value,{1,a.hi});
                else if(a.hi==0)env.refine(i.arguments[0].value,{a.lo,-1});
            }
        }return;
    }
    if(i.result)env.assign(*i.result,evaluate(i,env));
}
inline Environment transfer_block(const ir::Block& block,Environment input){
    for(const auto& i:block.instructions)transfer(input,i);return input;
}
}
