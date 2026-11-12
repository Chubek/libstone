#include "../sentinel/ConcurrencyAnalysis.hpp"
#include "../integration/Moonstone.hpp"
#include <cassert>
using namespace moonstone::plaid;
int main(){
    sentinel::VectorClock a,b;
    a.tick(1);b.tick(2);
    assert(a.concurrent_with(b));
    auto found=sentinel::races({{1,7,true,false,a},{2,7,false,false,b}});
    assert(found.size()==1);
    assert(sentinel::races({{1,7,true,true,a},{2,7,false,false,b}}).size()==1);
    assert(sentinel::races({{1,7,true,true,a},{2,7,false,true,b}}).empty());
    auto ds=integration::race_diagnostics(found,"sample.c");
    assert(ds.size()==1 && ds[0].id=="PLAID-RACE");
    b.join(a);b.tick(2);
    assert(a.ordered_before(b));
    assert(sentinel::races({{1,7,true,false,a},{2,7,false,false,b}}).empty());
    sentinel::LockOrder locks;locks.add("a","b");locks.add("b","c");
    bool threw=false;try{locks.add("c","a");}catch(const std::logic_error&){threw=true;}assert(threw);
    assert(integration::check_migration({true,true,false},true).is_proven());
    assert(!integration::check_migration({false,true,false},true).is_proven());
}
