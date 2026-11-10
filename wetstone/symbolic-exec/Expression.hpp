#pragma once
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace wetstone::symbolic {
enum class Sort{Integer,Boolean};
enum class Kind{Integer,Boolean,Variable,Add,Sub,Mul,Div,Mod,Neg,Eq,Ne,Lt,Le,Gt,Ge,And,Or,Not};
struct Expr {
    Kind kind=Kind::Integer;Sort sort=Sort::Integer;
    std::int64_t literal=0;std::string symbol;
    std::shared_ptr<const Expr> lhs,rhs;
    static Expr integer(std::int64_t n){return {Kind::Integer,Sort::Integer,n,{},{},{}};}
    static Expr boolean(bool b){return {Kind::Boolean,Sort::Boolean,b?1:0,{},{},{}};}
    static Expr variable(std::string s,Sort sort=Sort::Integer){return {Kind::Variable,sort,0,std::move(s),{},{}};}
    static Expr unary(Kind k,Expr a){
        if(k!=Kind::Neg&&k!=Kind::Not)throw std::invalid_argument("not unary");
        if((k==Kind::Not)!=(a.sort==Sort::Boolean))throw std::invalid_argument("sort mismatch");
        return {k,a.sort,0,{},std::make_shared<Expr>(std::move(a)),{}};
    }
    static Expr binary(Kind k,Expr a,Expr b){
        if(a.sort!=b.sort)throw std::invalid_argument("sort mismatch");
        bool logical=k==Kind::And||k==Kind::Or;
        bool compare=k==Kind::Eq||k==Kind::Ne||k==Kind::Lt||k==Kind::Le||k==Kind::Gt||k==Kind::Ge;
        if(logical&&a.sort!=Sort::Boolean)throw std::invalid_argument("logical operator requires Boolean");
        if(!logical&&!compare&&a.sort!=Sort::Integer)throw std::invalid_argument("arithmetic requires integer");
        if((k==Kind::Lt||k==Kind::Le||k==Kind::Gt||k==Kind::Ge)&&a.sort!=Sort::Integer)
            throw std::invalid_argument("ordered comparison requires integer");
        return {k,(logical||compare)?Sort::Boolean:Sort::Integer,0,{},
            std::make_shared<Expr>(std::move(a)),std::make_shared<Expr>(std::move(b))};
    }
};
inline const char* smt_operator(Kind k){switch(k){
    case Kind::Add:return "+";case Kind::Sub:return "-";case Kind::Mul:return "*";
    case Kind::Div:return "div";case Kind::Mod:return "mod";case Kind::Neg:return "-";
    case Kind::Eq:return "=";case Kind::Ne:return "distinct";case Kind::Lt:return "<";
    case Kind::Le:return "<=";case Kind::Gt:return ">";case Kind::Ge:return ">=";
    case Kind::And:return "and";case Kind::Or:return "or";case Kind::Not:return "not";
    default:return "?";
}}
inline std::string format(const Expr& e){
    if(e.kind==Kind::Integer)return e.literal<0?"(- "+std::to_string(std::uint64_t{0}-static_cast<std::uint64_t>(e.literal))+")":std::to_string(e.literal);
    if(e.kind==Kind::Boolean)return e.literal?"true":"false";
    if(e.kind==Kind::Variable)return e.symbol;
    auto s=std::string("(")+smt_operator(e.kind)+" "+format(*e.lhs);
    if(e.rhs)s+=" "+format(*e.rhs);return s+")";
}
inline std::optional<std::int64_t> evaluate(const Expr& e,const std::map<std::string,std::int64_t>& bindings={}) {
    if(e.kind==Kind::Integer||e.kind==Kind::Boolean)return e.literal;
    if(e.kind==Kind::Variable){auto it=bindings.find(e.symbol);return it==bindings.end()?std::nullopt:std::optional<std::int64_t>(it->second);}
    auto a=evaluate(*e.lhs,bindings);if(!a)return std::nullopt;
    auto b=e.rhs?evaluate(*e.rhs,bindings):std::optional<std::int64_t>{};
    if(e.rhs&&!b)return std::nullopt;
    __int128 x=*a,y=b?*b:0,z=0;
    switch(e.kind){
        case Kind::Neg:z=-x;break;case Kind::Not:return !x;
        case Kind::Add:z=x+y;break;case Kind::Sub:z=x-y;break;case Kind::Mul:z=x*y;break;
        case Kind::Div:if(!y)return std::nullopt;z=x/y;break;
        case Kind::Mod:if(!y)return std::nullopt;z=x%y;break;
        case Kind::Eq:return x==y;case Kind::Ne:return x!=y;case Kind::Lt:return x<y;
        case Kind::Le:return x<=y;case Kind::Gt:return x>y;case Kind::Ge:return x>=y;
        case Kind::And:return bool(x)&&bool(y);case Kind::Or:return bool(x)||bool(y);
        default:return std::nullopt;
    }
    if(z<INT64_MIN||z>INT64_MAX)return std::nullopt;
    return static_cast<std::int64_t>(z);
}
}
