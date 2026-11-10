#pragma once
#include "ArithmeticSafety.hpp"
#include "../utils/AnalysisPipeline.hpp"
#include "../pointer-analysis/Ownership.hpp"
namespace wetstone::verification {
inline AnalysisPipeline standard_suite(){
    AnalysisPipeline p;
    p.register_analysis("arithmetic",[](const ir::Function& f,AnalysisManager&){return division_warnings(f);});
    p.register_analysis("assertions",[](const ir::Function& f,AnalysisManager&){auto r=check_assertions(f);
        Diagnostics all=r.definite_failures;all.insert(all.end(),r.possible_failures.begin(),r.possible_failures.end());return all;});
    p.register_analysis("ownership",[](const ir::Function& f,AnalysisManager&){return pointer::ownership_warnings(f);});
    return p;
}
}
