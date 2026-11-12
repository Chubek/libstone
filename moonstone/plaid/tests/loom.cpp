#include "../loom/Scheduler.hpp"
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <vector>
using namespace moonstone::plaid::loom;
int main() {
    CooperativeScheduler scheduler;
    int observed=0;
    scheduler.post([&]{observed=1;scheduler.post([&]{observed=2;});});
    assert(scheduler.run(1)==1 && observed==1 && scheduler.pending()==1);
    assert(scheduler.run()==1 && observed==2);
    WorkStealingPool pool(4);
    std::vector<std::future<int>> jobs;
    for(int i=0;i<1000;++i)jobs.push_back(pool.submit([i]{return i*2;}));
    long long sum=0;for(auto& f:jobs)sum+=f.get();assert(sum==999000);
    std::atomic<int> count{0};
    {TaskScope scope(pool);for(int i=0;i<100;++i)scope.spawn([&](const CancellationToken& t){if(!t.requested()) count.fetch_add(1);});scope.join();}
    assert(count==100);
    auto bad=pool.submit([]()->void{throw std::logic_error("task failure");});
    bool threw=false;try{bad.get();}catch(const std::logic_error&){threw=true;}assert(threw);
    pool.stop();
    threw=false;try{pool.submit([]{});}catch(const std::runtime_error&){threw=true;}assert(threw);
}
