#pragma once
#include "Expression.hpp"
namespace wetstone::symbolic {
using PathCondition=std::vector<Expr>;
inline Expr logical_not(Expr e){return Expr::unary(Kind::Not,std::move(e));}
inline Expr logical_and(Expr a,Expr b){return Expr::binary(Kind::And,std::move(a),std::move(b));}
inline Expr eq(Expr a,Expr b){return Expr::binary(Kind::Eq,std::move(a),std::move(b));}
}
