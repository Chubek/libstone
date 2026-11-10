#pragma once
#include "AbstractDomain.hpp"
namespace wetstone::ai {
template<AbstractDomain A,AbstractDomain B>struct Product {
    A first=A::top();B second=B::top();
    static Product top(){return {A::top(),B::top()};}
    static Product bottom(){return {A::bottom(),B::bottom()};}
    bool leq(const Product& b)const{return first.leq(b.first)&&second.leq(b.second);}
    Product join(const Product& b)const{return {first.join(b.first),second.join(b.second)};}
    Product meet(const Product& b)const{return {first.meet(b.first),second.meet(b.second)};}
    Product widen(const Product& b)const{return {first.widen(b.first),second.widen(b.second)};}
    friend bool operator==(const Product&,const Product&)=default;
};
}
