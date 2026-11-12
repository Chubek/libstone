#include "../conduit/Channel.hpp"
#include "../conduit/Select.hpp"
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
using namespace moonstone::plaid::conduit;
using namespace std::chrono_literals;
int main() {
    Channel<int> c(2);
    assert(c.try_send(4) && c.try_send(5) && !c.try_send(6));
    assert(c.receive()==4 && c.receive()==5);
    c.close(); assert(!c.send(7) && !c.receive());
    Channel<int> rendezvous(0);
    std::thread sender([&]{assert(rendezvous.send(42));});
    assert(rendezvous.receive()==42);
    sender.join(); rendezvous.close();
    Channel<int> a(1),b(1);
    assert(b.send(19));
    auto got=select_receive<int>({&a,&b},1s);
    assert(got && got->index==1 && got->value==19 && !got->closed);
    auto timeout=select_receive<int>({&a,&b},2ms);assert(!timeout);
    a.close(); auto closed=select_receive<int>({&a,&b},1s);
    assert(closed && closed->closed && closed->index==0);
    bool rejected=false;
    try{(void)select_receive<int>({&rendezvous},1ms);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
}
