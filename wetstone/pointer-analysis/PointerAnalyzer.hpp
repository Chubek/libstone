#pragma once
#include "PointsTo.hpp"
#include <limits>
namespace wetstone::pointer {
struct PointerAnalyzer {
    std::map<std::string,ObjectId> objects;
    std::vector<InclusionConstraint> constraints;
    ObjectId object(const std::string& name){auto i=objects.find(name);if(i!=objects.end())return i->second;
        if(objects.size()>=UINT32_MAX-1)throw std::overflow_error("object ID space");
        auto id=static_cast<ObjectId>(objects.size());objects[name]=id;return id;}
    void address(ir::ValueId result,const std::string& name){constraints.push_back({InclusionConstraint::Kind::Address,result,0,object(name)});}
    void copy(ir::ValueId dst,ir::ValueId src){constraints.push_back({InclusionConstraint::Kind::Copy,dst,src,{}});}
    void load(ir::ValueId dst,ir::ValueId ptr){constraints.push_back({InclusionConstraint::Kind::Load,dst,ptr,{}});}
    void store(ir::ValueId ptr,ir::ValueId src){constraints.push_back({InclusionConstraint::Kind::Store,ptr,src,{}});}
    PointsToResult solve(std::size_t limit=10000)const{return andersen(constraints,limit);}
    static PointerAnalyzer build(const ir::Function& f){
        PointerAnalyzer a;
        for(auto p:f.parameters)a.constraints.push_back({InclusionConstraint::Kind::Address,p,0,unknown_object});
        for(const auto& b:f.blocks)for(std::size_t n=0;n<b.instructions.size();++n){
            const auto& i=b.instructions[n];
            if((i.opcode==ir::Opcode::AddressOf||i.opcode==ir::Opcode::Alloc)&&i.result)
                a.address(*i.result,i.symbol.empty()?"alloc:"+std::to_string(b.id)+":"+std::to_string(n):i.symbol);
            else if(i.opcode==ir::Opcode::Copy&&i.result&&!i.arguments.empty()&&!i.arguments[0].is_constant)
                a.copy(*i.result,i.arguments[0].value);
            else if(i.opcode==ir::Opcode::Phi&&i.result){for(const auto& arg:i.arguments)
                if(!arg.is_constant)a.copy(*i.result,arg.value);}
            else if(i.opcode==ir::Opcode::Load&&i.result&&!i.arguments.empty()&&!i.arguments[0].is_constant)
                a.load(*i.result,i.arguments[0].value);
            else if(i.opcode==ir::Opcode::Store&&i.arguments.size()>=2&&!i.arguments[0].is_constant&&!i.arguments[1].is_constant)
                a.store(i.arguments[0].value,i.arguments[1].value);
            else if(i.opcode==ir::Opcode::Call&&i.result)
                a.constraints.push_back({InclusionConstraint::Kind::Address,*i.result,0,unknown_object});
        }return a;
    }
};
}
