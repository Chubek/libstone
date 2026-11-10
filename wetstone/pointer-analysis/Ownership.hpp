#pragma once
#include "../utils/Diagnostic.hpp"
#include "../utils/IR.hpp"
#include <set>
namespace wetstone::pointer {
// Local syntactic double-release scan; this is a warning, not a proof of UAF.
inline Diagnostics ownership_warnings(const ir::Function& f){
    Diagnostics r;
    for(const auto& b:f.blocks){std::set<ir::ValueId> freed;
        for(const auto& i:b.instructions){if(i.opcode==ir::Opcode::Free&&!i.arguments.empty()&&!i.arguments[0].is_constant){
            auto p=i.arguments[0].value;
            if(!freed.insert(p).second)r.push_back({"WET-DOUBLE-FREE","repeated free of the same SSA pointer in one block","",f.name,i.source_line,b.id,Severity::Warning});
        }}
    }return r;
}
}
