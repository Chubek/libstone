#include <TTyUtils/TTyUtils-Multiplex.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::mux;
    Server server;
    Client first;
    auto* saved = first.attach_or_create(server, "work");
    saved->window(0).split(Pane{"editor"});
    detach(*saved);
    for (int i = 0; i < 100; ++i) server.create(std::to_string(i));
    Client second;
    assert(second.attach_or_create(server, "work") == saved);
    assert(saved->window(0).panes().front().command() == "editor");
    assert(server.sessions().size() == 101);
}
