#include <TTyUtils/TTyUtils-SessionShare.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::share;
    ttyutils::p2p::Node node;
    Host host{node};
    assert(host.invite(Permissions::ReadWrite).empty());
    for (const auto* token : {"", "readwrite", "forged"}) {
        const auto guest = Guest::join(token);
        assert(!guest.connected());
        assert(guest.permission() == Permissions::ReadOnly);
    }
}
