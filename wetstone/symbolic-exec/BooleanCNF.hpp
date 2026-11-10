#pragma once
#include "Constraint.hpp"
#include <map>
#include <stdexcept>
namespace wetstone::symbolic {
// Tseitin encoding of Boolean expressions. Integer arithmetic comparisons of
// unknown operands are *not* Booleanized as independent atoms (unsound).
struct BooleanCNF {
    std::vector<std::vector<std::int32_t>> clauses;
    std::map<std::string,std::int32_t> variables;
    std::int32_t count=0;
    bool supported=true;
    std::int32_t fresh(){if(count>=INT32_MAX-1)throw std::overflow_error("SAT variables");return ++count;}
    std::int32_t atom(const std::string& s){auto it=variables.find(s);if(it!=variables.end())return it->second;auto v=fresh();variables[s]=v;return v;}
    std::int32_t encode(const Expr& e){
        if(e.sort!=Sort::Boolean){supported=false;return fresh();}
        if(e.kind==Kind::Boolean){auto v=fresh();clauses.push_back({e.literal?v:-v});return v;}
        if(e.kind==Kind::Variable)return atom(e.symbol);
        if(e.kind==Kind::Not)return -encode(*e.lhs);
        if(e.kind==Kind::And||e.kind==Kind::Or){
            auto a=encode(*e.lhs),b=encode(*e.rhs),out=fresh();
            if(e.kind==Kind::And){clauses.push_back({-out,a});clauses.push_back({-out,b});clauses.push_back({out,-a,-b});}
            else {clauses.push_back({out,-a});clauses.push_back({out,-b});clauses.push_back({-out,a,b});}
            return out;
        }
        if(e.kind==Kind::Eq||e.kind==Kind::Ne){
            if(e.lhs->sort==Sort::Integer){auto v=evaluate(e);if(!v){supported=false;return fresh();}
                auto id=fresh();clauses.push_back({*v?id:-id});return id;}
            auto a=encode(*e.lhs),b=encode(*e.rhs),out=fresh();
            // out <-> (a == b) or its negation, allowing a/b to be signed literals.
            const bool neq=e.kind==Kind::Ne;
            clauses.push_back({neq?-out:out,-a,-b});clauses.push_back({neq?-out:out,a,b});
            clauses.push_back({neq?out:-out,-a,b});clauses.push_back({neq?out:-out,a,-b});
            return out;
        }
        auto v=evaluate(e);if(v){auto n=fresh();clauses.push_back({*v?n:-n});return n;}
        supported=false;return fresh();
    }
    void assert_true(const Expr& e){auto v=encode(e);clauses.push_back({v});}
};
}
