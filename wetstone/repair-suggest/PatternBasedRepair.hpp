#pragma once
#include "Sink.hpp"
#include "../abstract-interp/IntervalAnalysis.hpp"
#include "../pointer-analysis/Ownership.hpp"
namespace wetstone::repair {
inline Sink pattern_based(const ir::Function& f,std::size_t budget=100000){
    Sink sink;auto analysis=ai::analyze_intervals(f,budget);
    for(const auto& b:f.blocks){
      auto env=analysis.in.at(b.id);
      for(const auto& i:b.instructions){
        if((i.opcode==ir::Opcode::Div||i.opcode==ir::Opcode::Mod)&&i.arguments.size()>1){
            auto denom=ai::operand(env,i.arguments[1]);
            if(denom.contains(0))sink.emit({{"WET-DIVZERO","divisor may be zero","",f.name,i.source_line,b.id},
                "Potential arithmetic trap or undefined division", "Guard denominator != 0 before division", false});
        }
        if(i.opcode==ir::Opcode::Assert&& !i.arguments.empty() && i.arguments[0].is_constant && !i.arguments[0].literal)
            sink.emit({{"WET-ASSERT","assert(false) is reachable syntactically","",f.name,i.source_line,b.id},
                "Assertion fails if block is reachable", "Fix asserted invariant or change control-flow",false});
        ai::transfer(env,i);
      }
    }
    for(auto d:pointer::ownership_warnings(f))sink.emit({d,"Possible repeated release of a resource",
        "Make release idempotent or transfer ownership",false});
    return sink;
}
}
