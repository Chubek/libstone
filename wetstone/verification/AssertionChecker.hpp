#pragma once
#include "../abstract-interp/IntervalAnalysis.hpp"
#include "../utils/Diagnostic.hpp"
namespace wetstone::verification {
struct AssertionReport {Diagnostics definite_failures,possible_failures;bool complete=true;};
inline AssertionReport check_assertions(const ir::Function& f,std::size_t budget=100000){
    auto analysis=ai::analyze_intervals(f,budget);AssertionReport report;report.complete=analysis.converged;
    for(const auto& b:f.blocks){
        auto it=analysis.in.find(b.id);if(it==analysis.in.end()||it->second.is_bottom())continue;
        auto state=it->second;
        for(const auto& inst:b.instructions){
            if(inst.opcode==ir::Opcode::Assert&&!inst.arguments.empty()){
                auto value=ai::operand(state,inst.arguments[0]);
                Diagnostic d{"WET-ASSERT","assertion may fail","",f.name,inst.source_line,b.id,Severity::Warning};
                if(value==ai::Interval::point(0)){d.message="assertion definitely fails";d.severity=Severity::Error;report.definite_failures.push_back(d);}
                else if(value.contains(0))report.possible_failures.push_back(d);
            }
            if(inst.opcode!=ir::Opcode::Assert)ai::transfer(state,inst);
        }
    }return report;
}
}
