#pragma once
#include "PointerAnalyzer.hpp"
namespace wetstone::pointer {
struct EscapeResult {std::set<ObjectId> escaping;bool unknown=false;};
inline EscapeResult escapes(const ir::Function& f,const PointsToResult& p){
    EscapeResult r;
    for(const auto& b:f.blocks)for(const auto& i:b.instructions){
        auto escape=[&](ir::Operand op){if(op.is_constant)return;
            for(auto obj:p.points_to(op.value)){if(obj==unknown_object)r.unknown=true;else r.escaping.insert(obj);}
        };
        if(i.opcode==ir::Opcode::Return||i.opcode==ir::Opcode::Call)
            for(const auto& o:i.arguments)escape(o);
        if(i.opcode==ir::Opcode::Store&&i.arguments.size()>=2){
            if(i.arguments[0].is_constant||p.points_to(i.arguments[0].value).count(unknown_object))escape(i.arguments[1]);
        }
    }return r;
}
}
