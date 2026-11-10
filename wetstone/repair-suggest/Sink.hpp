#pragma once
#include "../utils/Diagnostic.hpp"
#include <string>
#include <vector>
namespace wetstone::repair {
struct Suggestion {
    Diagnostic problem;
    std::string explanation,proposed_change;
    bool automatically_safe=false;
};
class Sink {
    std::vector<Suggestion> suggestions_;
public:
    void emit(Suggestion s){suggestions_.push_back(std::move(s));}
    const std::vector<Suggestion>& suggestions()const{return suggestions_;}
    bool empty()const{return suggestions_.empty();}
};
}
