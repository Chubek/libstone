#pragma once
#include "IR.hpp"
namespace wetstone::ir {
struct Program {
    std::vector<Function> functions;
    const Function* lookup(const std::string& name)const{
        for(const auto& f:functions)if(f.name==name)return &f;return nullptr;
    }
    void validate()const{
        std::set<std::string> names;
        for(const auto& f:functions){if(f.name.empty()||!names.insert(f.name).second)
            throw std::invalid_argument("duplicate or empty function name");f.validate();}
    }
};
}
