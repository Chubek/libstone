#pragma once
#include "Solver.hpp"
#include "../utils/IR.hpp"
#include <queue>
namespace wetstone::symbolic {
struct State {
    ir::BlockId block=ir::invalid_block;
    ir::BlockId predecessor=ir::invalid_block;
    std::map<ir::ValueId,Expr> values;
    PathCondition constraints;
    std::vector<ir::BlockId> trace;
    std::optional<Expr> returned;
};
struct ExecutorResult {
    std::vector<State> completed;
    std::size_t explored=0;bool truncated=false;
};
inline Expr operand(const State& s,const ir::Operand& a){
    if(a.is_constant)return Expr::integer(a.literal);
    if(auto it=s.values.find(a.value);it!=s.values.end())return it->second;
    return Expr::variable("v"+std::to_string(a.value));
}
inline Expr lift(const ir::Instruction& i,const State& s){
    auto get=[&](std::size_t n){return n<i.arguments.size()?operand(s,i.arguments[n]):Expr::variable("undefined");};
    using ir::Opcode;using K=Kind;
    if(i.opcode==Opcode::Input)return Expr::variable("input"+std::to_string(*i.result));
    if(i.opcode==Opcode::Constant||i.opcode==Opcode::Copy)return get(0);
    if(i.opcode==Opcode::Phi){
        for(std::size_t n=0;n<i.phi_predecessors.size();++n)if(i.phi_predecessors[n]==s.predecessor)return get(n);
        return Expr::variable("phi"+std::to_string(*i.result));
    }
    if(i.opcode==Opcode::Neg)return Expr::unary(K::Neg,get(0));
    K op;switch(i.opcode){
        case Opcode::Add:op=K::Add;break;case Opcode::Sub:op=K::Sub;break;case Opcode::Mul:op=K::Mul;break;
        case Opcode::Div:op=K::Div;break;case Opcode::Mod:op=K::Mod;break;
        case Opcode::Eq:op=K::Eq;break;case Opcode::Ne:op=K::Ne;break;
        case Opcode::Lt:op=K::Lt;break;case Opcode::Le:op=K::Le;break;
        case Opcode::Gt:op=K::Gt;break;case Opcode::Ge:op=K::Ge;break;
        default:return Expr::variable("unknown"+std::to_string(i.result.value_or(0)));
    }return Expr::binary(op,get(0),get(1));
}
inline Expr as_boolean(Expr e){if(e.sort==Sort::Boolean)return e;return Expr::binary(Kind::Ne,std::move(e),Expr::integer(0));}
inline ExecutorResult execute(const ir::Function& f,Solver* solver=nullptr,
                             std::size_t state_budget=1000,std::size_t depth_limit=48){
    f.validate();ExecutorResult r;State entry;entry.block=f.entry;
    for(auto p:f.parameters)entry.values.emplace(p,Expr::variable("arg"+std::to_string(p)));
    std::queue<State> pending;pending.push(std::move(entry));
    auto feasible=[&](const PathCondition& constraints){
        for(const auto& c:constraints)if(auto v=evaluate(c);v&&!*v)return false;
        return !solver||solver->check(constraints).status!=CheckResult::Unsat;
    };
    while(!pending.empty()){
        if(r.explored>=state_budget){r.truncated=true;break;}
        State s=std::move(pending.front());pending.pop();++r.explored;
        if(s.trace.size()>=depth_limit){r.truncated=true;continue;}
        auto b=s.block;s.trace.push_back(b);
        bool terminated=false,returned=false;
        for(const auto& i:f.block(b).instructions){
            if(i.result)s.values.insert_or_assign(*i.result,lift(i,s));
            if((i.opcode==ir::Opcode::Assume||i.opcode==ir::Opcode::Assert)&&!i.arguments.empty()){
                if(i.opcode==ir::Opcode::Assume)s.constraints.push_back(as_boolean(operand(s,i.arguments[0])));
            }
            if(i.opcode==ir::Opcode::Return){if(!i.arguments.empty())s.returned=operand(s,i.arguments[0]);returned=true;terminated=true;break;}
            if(i.opcode==ir::Opcode::Jump){auto next=s;next.predecessor=b;next.block=i.targets.at(0);
                if(feasible(next.constraints))pending.push(std::move(next));terminated=true;break;}
            if(i.opcode==ir::Opcode::Branch){
                auto condition=as_boolean(operand(s,i.arguments.at(0)));
                for(std::size_t side=0;side<2;++side){auto next=s;next.predecessor=b;
                    next.block=i.targets.at(side);next.constraints.push_back(side==0?condition:logical_not(condition));
                    if(feasible(next.constraints))pending.push(std::move(next));
                }terminated=true;break;
            }
        }
        if(!terminated&&feasible(s.constraints))r.completed.push_back(std::move(s));
        else if(returned&&feasible(s.constraints))r.completed.push_back(std::move(s));
    }return r;
}
}
