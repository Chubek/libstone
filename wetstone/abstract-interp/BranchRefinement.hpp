#pragma once
#include "Transfer.hpp"
namespace wetstone::ai {
// Refine one interval operand of a comparison with a constant. No unsound
// narrowing when both sides are symbolic or comparisons are unrecognized.
inline Environment refine_compare(Environment e,const ir::Instruction& cmp,bool truth){
    using ir::Opcode;auto op=cmp.opcode;
    if(cmp.arguments.size()!=2)return e;
    auto left=cmp.arguments[0],right=cmp.arguments[1];
    if(left.is_constant==right.is_constant)return e;
    if(left.is_constant){std::swap(left,right);
        switch(op){case Opcode::Lt:op=Opcode::Gt;break;case Opcode::Le:op=Opcode::Ge;break;
            case Opcode::Gt:op=Opcode::Lt;break;case Opcode::Ge:op=Opcode::Le;break;default:break;}}
    auto k=right.literal;
    switch(op){
        case Opcode::Eq:if(truth)e.refine(left.value,Interval::point(k));break;
        case Opcode::Ne:if(!truth)e.refine(left.value,Interval::point(k));break;
        case Opcode::Lt:if(k==INT64_MIN&&truth)e=Environment::bottom();else e.refine(left.value,truth?Interval{INT64_MIN,k==INT64_MIN?INT64_MIN:k-1}:Interval{k,INT64_MAX});break;
        case Opcode::Le:if(k==INT64_MAX&&!truth)e=Environment::bottom();else e.refine(left.value,truth?Interval{INT64_MIN,k}:Interval{k==INT64_MAX?INT64_MAX:k+1,INT64_MAX});break;
        case Opcode::Gt:if(k==INT64_MAX&&truth)e=Environment::bottom();else e.refine(left.value,truth?Interval{k==INT64_MAX?INT64_MAX:k+1,INT64_MAX}:Interval{INT64_MIN,k});break;
        case Opcode::Ge:if(k==INT64_MIN&&!truth)e=Environment::bottom();else e.refine(left.value,truth?Interval{k,INT64_MAX}:Interval{INT64_MIN,k==INT64_MIN?INT64_MIN:k-1});break;
        default:break;
    }
    return e;
}
inline Environment refine_branch(const ir::Block& b,Environment e,bool taken_true){
    if(b.instructions.empty())return e;
    const auto& term=b.instructions.back();
    if(term.opcode!=ir::Opcode::Branch||term.arguments.empty())return e;
    auto cond=term.arguments[0];
    if(cond.is_constant)return (static_cast<bool>(cond.literal)==taken_true)?e:Environment::bottom();
    for(const auto& i:b.instructions)if(i.result&&*i.result==cond.value)
        return refine_compare(std::move(e),i,taken_true);
    return e;
}
}
