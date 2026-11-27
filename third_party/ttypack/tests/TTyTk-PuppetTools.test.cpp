#include <TTyTk/TTyTk-PuppetTools.hpp>
#include <cassert>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    ttytk::Puppet puppet;
    assert(!puppet.wait_for_text("ready", 2ms));
    std::jthread publisher([&] {
        puppet.observe("\x1b[");
        puppet.observe("31mrea");
        puppet.observe("\x1b[0mdy");
    });
    assert(puppet.wait_for_text("ready", 2s));
    assert(!puppet.wait_for_text("31m"));
    puppet.send_key({U'界', 4, 3});
    puppet.send_mouse({64, 4, 10, 20, true, false});
    puppet.send_mouse({3, 0, 10, 20, false, true});
    puppet.send_mouse({0, 0, 0, 1, true, false});
    assert(puppet.sent() == "\x1b[30028;5:3u\x1b[<68;10;20M\x1b[<35;10;20M");
}
