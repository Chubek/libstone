#pragma once
#include "Constraint.hpp"
#include <set>
#include <sstream>
namespace wetstone::symbolic {
inline void collect_variables(const Expr& e,std::map<std::string,Sort>& out){
    if(e.kind==Kind::Variable){auto it=out.find(e.symbol);if(it!=out.end()&&it->second!=e.sort)throw std::invalid_argument("sort conflict");out[e.symbol]=e.sort;}
    if(e.lhs)collect_variables(*e.lhs,out);if(e.rhs)collect_variables(*e.rhs,out);
}
inline std::string smtlib2(const PathCondition& conditions){
    std::map<std::string,Sort> vars;for(const auto& e:conditions){if(e.sort!=Sort::Boolean)throw std::invalid_argument("assert expects Boolean");collect_variables(e,vars);}
    std::ostringstream out;out<<"(set-logic ALL)\n";
    for(const auto& [key,sort]:vars){if(key.empty()||key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.$")!=std::string::npos)
        throw std::invalid_argument("unsafe SMT symbol");
        out<<"(declare-const "<<key<<" "<<(sort==Sort::Boolean?"Bool":"Int")<<")\n";}
    for(const auto& e:conditions)out<<"(assert "<<format(e)<<")\n";
    out<<"(check-sat)\n(get-model)\n";return out.str();
}
}
