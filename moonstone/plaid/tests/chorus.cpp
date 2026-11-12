#include "../chorus/Actor.hpp"
#include <atomic>
#include <cassert>
#include <vector>
using namespace moonstone::plaid;
int main(){
    loom::WorkStealingPool pool(4);
    std::vector<int> received;
    auto a=chorus::Actor<int>::create(pool,[&](int msg){received.push_back(msg);});
    for(int i=0;i<1000;++i) assert(a->send(i));
    a->close_and_wait();
    assert(received.size()==1000);
    for(int i=0;i<1000;++i)assert(received[i]==i);
    assert(!a->send(1001));
    auto failing=chorus::Actor<int>::create(pool,[](int){throw std::runtime_error("handler failed");});
    assert(failing->send(1));
    bool failed=false;try{failing->close_and_wait();}catch(const std::runtime_error&){failed=true;}assert(failed);
}
