#include "../Plaid.hpp"
#include <iostream>
#include <vector>
int main() {
    using namespace moonstone::plaid;
    loom::WorkStealingPool pool(4);
    std::vector<long long> values(1000);
    parallax::parallel_for(pool,0,values.size(),100,[&](std::size_t i){values[i]=static_cast<long long>(i);});
    auto sum=parallax::parallel_reduce<long long>(pool,0,values.size(),100,0,
        [&](std::size_t i){return values[i];},std::plus<>{});
    std::cout << sum << '\n';
}
