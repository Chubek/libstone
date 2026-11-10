#pragma once
#include "../abstract-interp/Constant.hpp"
#include "../utils/Graph.hpp"
#include <map>
#include <set>
namespace wetstone::df {
struct SCCPResult {
    std::map<ir::ValueId,ai::Constant> values;
    std::set<ir::BlockId> executable;
    std::set<std::pair<ir::BlockId,ir::BlockId>> edges;
    bool converged=true;std::size_t iterations=0;
    ai::Constant value(ir::Operand x)const {
        if(x.is_constant)return ai::Constant::point(x.literal);
        auto i=values.find(x.value);return i==values.end()?ai::Constant::bottom():i->second;
    }
};
inline ai::Constant sccp_evaluate(const ir::Instruction& i,const SCCPResult& r){
    using ai::Constant;using ir::Opcode;
    auto v=[&](std::size_t n){return n<i.arguments.size()?r.value(i.arguments[n]):Constant::top();};
    if(i.opcode==Opcode::Input||i.opcode==Opcode::Load||i.opcode==Opcode::Call)return Constant::top();
    if(i.opcode==Opcode::Phi){auto acc=Constant::bottom();for(auto a:i.arguments)acc=acc.join(r.value(a));return acc;}
    auto a=v(0),b=v(1);
    if(i.opcode==Opcode::Constant||i.opcode==Opcode::Copy)return a;
    if(a.kind==Constant::Kind::Bottom|| (i.opcode!=Opcode::Neg&&b.kind==Constant::Kind::Bottom))return Constant::bottom();
    if(a.kind!=Constant::Kind::Value|| (i.opcode!=Opcode::Neg&&b.kind!=Constant::Kind::Value))return Constant::top();
    __int128 n=a.value,m=b.value,z=0;
    switch(i.opcode){
        case Opcode::Neg:z=-n;break;case Opcode::Add:z=n+m;break;case Opcode::Sub:z=n-m;break;
        case Opcode::Mul:z=n*m;break;case Opcode::Div:if(m==0)return Constant::top();z=n/m;break;
        case Opcode::Mod:if(m==0)return Constant::top();z=n%m;break;
        case Opcode::Eq:return Constant::point(n==m);case Opcode::Ne:return Constant::point(n!=m);
        case Opcode::Lt:return Constant::point(n<m);case Opcode::Le:return Constant::point(n<=m);
        case Opcode::Gt:return Constant::point(n>m);case Opcode::Ge:return Constant::point(n>=m);
        case Opcode::BitAnd:return Constant::point(a.value&b.value);
        case Opcode::BitOr:return Constant::point(a.value|b.value);
        case Opcode::BitXor:return Constant::point(a.value^b.value);
        default:return Constant::top();
    }
    if(z<INT64_MIN||z>INT64_MAX)return Constant::top();
    return Constant::point(static_cast<std::int64_t>(z));
}
inline SCCPResult sccp(const ir::Function& f,std::size_t limit=100000){
    f.validate();SCCPResult r;r.executable.insert(f.entry);
    for(auto p:f.parameters)r.values[p]=ai::Constant::top();
    bool changed=true;
    while(changed){if(r.iterations++>=limit){r.converged=false;break;}changed=false;
        for(const auto& block:f.blocks)if(r.executable.count(block.id)){
            for(const auto& i:block.instructions){
                if(i.result){auto now=sccp_evaluate(i,r);auto& old=r.values.try_emplace(*i.result,ai::Constant::bottom()).first->second;auto merged=old.join(now);
                    if(!(merged==old)){old=merged;changed=true;}}
                if(i.opcode==ir::Opcode::Jump||i.opcode==ir::Opcode::Branch){
                    std::vector<ir::BlockId> next=i.targets;
                    if(i.opcode==ir::Opcode::Branch){auto cond=r.value(i.arguments.at(0));
                        if(cond.kind==ai::Constant::Kind::Value)next={i.targets[cond.value?0:1]};
                        else if(cond.kind==ai::Constant::Kind::Bottom)next.clear();
                    }
                    for(auto b:next){if(r.edges.insert({block.id,b}).second)changed=true;
                        if(r.executable.insert(b).second)changed=true;}
                }
            }
        }
    }return r;
}
}
