#include "../Plaid.hpp"
#include <iostream>
int main(){
    using namespace moonstone::plaid;
    loom::CooperativeScheduler scheduler;
    continuum::Prompt prompt;
    auto k=prompt.capture<int>([](int result){std::cout<<"resumed with "<<result<<'\n';});
    scheduler.post([k]() mutable { (void)k.resume(42); });
    scheduler.run();
}
