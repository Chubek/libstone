#include <TTyTk/TTyTk-SignalTools.hpp>

#include <cassert>
#include <csignal>
#include <poll.h>

int main() {
    int interrupts = 0;
    int resizes = 0;
    assert(ttytk::signals::on_sigint([&] { ++interrupts; }));
    assert(ttytk::signals::on_sigwinch([&] { ++resizes; }));
    assert(::raise(SIGINT) == 0);
    assert(::raise(SIGWINCH) == 0);
    assert(!interrupts && !resizes);
    pollfd fd{ttytk::signals::notification_fd(), POLLIN, 0};
    assert(::poll(&fd, 1, 0) == 1);
    ttytk::signals::dispatch_pending();
    assert(interrupts == 1 && resizes == 1);
    assert(::poll(&fd, 1, 0) == 0);
    assert(ttytk::signals::on_sigint({}));
    assert(ttytk::signals::on_sigwinch({}));
}
