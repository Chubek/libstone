#include "../continuum/Continuation.hpp"
#include "../continuum/Effects.hpp"
#include <cassert>
#include <atomic>
#include <thread>
#include <vector>
using namespace moonstone::plaid::continuum;
int main() {
    std::atomic<int> result{0};
    OneShot<int> k([&](int x){result.fetch_add(x);});
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int i=0;i<8;++i) threads.emplace_back([&] { if(k.resume(1)==ResumeResult::Resumed) accepted.fetch_add(1); });
    for(auto& t:threads)t.join();
    assert(accepted==1 && result==1 && k.state()==State::Completed);
    auto cancelled=OneShot<int>([](int){});
    assert(cancelled.cancel() && cancelled.resume(1)==ResumeResult::Cancelled);
    auto failed=OneShot<int>([](int){throw std::runtime_error("err");});
    assert(failed.resume(2)==ResumeResult::Failed && failed.state()==State::Failed);
    bool threw=false;try { failed.rethrow_if_failed(); } catch(const std::runtime_error&) { threw=true; }assert(threw);
    OneShot<int> invalid;
    {Prompt prompt;invalid=prompt.capture<int>([](int){});}
    assert(invalid.resume(2)==ResumeResult::Failed);
    EffectScope root;root.bind("inc",[](const Effect& e)->std::any{return std::any_cast<int>(e.payload)+1;});
    EffectScope child(&root);assert(std::any_cast<int>(child.perform({"inc",3}))==4);
    threw=false;try {child.perform({"missing",{}});}catch(const std::runtime_error&){threw=true;}assert(threw);
}
