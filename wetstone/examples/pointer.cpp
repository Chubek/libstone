#include "Wetstone.hpp"
#include <iostream>
int main(){
    using namespace wetstone::pointer;
    PointerAnalyzer a;a.address(1,"A");a.address(2,"B");a.copy(3,1);
    auto result=a.solve();
    std::cout<<"p1 and p3 may alias: "<<result.may_alias(1,3)<<"\n";
    std::cout<<"p1 and p2 may alias: "<<result.may_alias(1,2)<<"\n";
}
