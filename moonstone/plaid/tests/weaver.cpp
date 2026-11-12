#include "../weaver/AsyncAnalysis.hpp"
#include "../loom/Coroutine.hpp"
#include <cassert>
#include <coroutine>
#include <algorithm>
using namespace moonstone::plaid;
loom::ManualCoroutine<int> count() {
    co_await std::suspend_always{};
    co_await std::suspend_always{};
    co_return 42;
}
int main() {
    using namespace weaver;
    AsyncFunction f{0,{
        {0,{{{}, {"x","z"}, false},{{"x"},{"y"},true}}, {1}},
        {1,{{{"y"},{"out"},false}}, {}}
    }};
    auto layout=analyze_frame(f);
    assert(layout.suspensions.size()==1);
    assert(layout.suspensions.front().saved_values.count("y")==1);
    assert(layout.suspensions.front().saved_values.count("x")==0);
    assert(layout.suspensions.front().saved_values.count("z")==0);
    auto c=count();assert(!c.done());assert(c.resume());assert(c.resume());assert(!c.resume());assert(c.result()==42);
    bool rejected=false;
    try{(void)analyze_frame({0,{{0,{}, {7}}}});}catch(const std::invalid_argument&){rejected=true;}
    assert(rejected);
}
