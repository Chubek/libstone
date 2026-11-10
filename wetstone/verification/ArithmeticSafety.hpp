#pragma once
#include "AssertionChecker.hpp"
namespace wetstone::verification {
inline Diagnostics division_warnings(const ir::Function& f,std::size_t budget=100000){
    auto analysis=ai::analyze_intervals(f,budget);Diagnostics results;
    for(const auto& b:f.blocks){
        auto it=analysis.in.find(b.id);if(it==analysis.in.end()||it->second.is_bottom())continue;
        auto state=it->second;
        for(const auto& i:b.instructions){
            if((i.opcode==ir::Opcode::Div||i.opcode==ir::Opcode::Mod)&&i.arguments.size()>=2){
                auto divisor=ai::operand(state,i.arguments[1]);
                if(divisor.contains(0))results.push_back({"WET-DIVZERO",divisor==ai::Interval::point(0)?"division by zero":"division may be by zero",
                    "",f.name,i.source_line,b.id,divisor==ai::Interval::point(0)?Severity::Error:Severity::Warning});
            }ai::transfer(state,i);
        }
    }return results;
}
}
