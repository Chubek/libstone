#include "../parallax/Parallel.hpp"
#include "../parallax/Dependence.hpp"
#include <atomic>
#include <cassert>
#include <vector>
using namespace moonstone::plaid;
int main(){
    loom::WorkStealingPool pool(4);
    std::vector<int> data(1000);
    parallax::parallel_for(pool,0,data.size(),13,[&](std::size_t i){data[i]=int(i);});
    assert(data[999]==999);
    auto sum=parallax::parallel_reduce<long long>(pool,0,1000,27,0,
        [](std::size_t i){return static_cast<long long>(i);},std::plus<>{});
    assert(sum==499500);
    std::vector<int> out(1000);
    parallax::lane_map<int,8>(data.data(),out.data(),out.size(),[](int x){return x+1;});
    assert(out[0]==1 && out[999]==1000);
    using namespace parallax;
    assert(loop_dependence({{1,0,4,4,true}},100).status==Dependence::Independent);
    assert(loop_dependence({{1,0,0,4,true}},5).status==Dependence::Dependent);
    assert(loop_dependence({{1,0,4,4,true}},1000,100).status==Dependence::Unknown);
}
