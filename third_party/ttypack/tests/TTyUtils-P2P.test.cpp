#include <TTyUtils/TTyUtils-P2P.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::p2p;
    Discovery discovery;
    discovery.announce({"friend"});
    discovery.announce({"friend"});
    discovery.announce({});
    assert(discovery.peers().size() == 1);
    Node node({"local"});
    auto channel = node.dial(discovery.peers().front());
    assert(!channel.connected());
    channel.send("local frame");
    assert(channel.frames().front() == "local frame");
}
