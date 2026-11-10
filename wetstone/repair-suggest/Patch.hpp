#pragma once
#include "Sink.hpp"
#include <sstream>
namespace wetstone::repair {
inline std::string report(const Sink& sink){
    std::ostringstream o;
    for(const auto& s:sink.suggestions())o<<s.problem.rule<<": "<<s.problem.message
        <<"\n  rationale: "<<s.explanation<<"\n  suggested fix: "<<s.proposed_change<<"\n";
    return o.str();
}
}
