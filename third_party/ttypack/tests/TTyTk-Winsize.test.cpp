#include <TTyTk/TTyTk-Winsize.hpp>

#include <cassert>
#include <csignal>
#include <limits>

int main() {
    bool changed = false;
    const auto installed = ttytk::winsize::on_change([&changed] { changed = true; });
    assert(installed);
    bool also_changed = false;
    assert(ttytk::signals::on_sigwinch([&] { also_changed = true; }));
    assert(::raise(SIGWINCH) == 0);
    assert(!changed && !also_changed);
    const auto size = ttytk::winsize::get(-1);
    assert(!size);
    assert(changed && also_changed);
    changed = also_changed = false;
    assert(ttytk::winsize::on_change({}));
    assert(::raise(SIGWINCH) == 0);
    ttytk::signals::dispatch_pending();
    assert(!changed && also_changed);
    assert(ttytk::signals::on_sigwinch({}));
    const auto huge = ttytk::winsize::set(-1, {std::numeric_limits<unsigned>::max(), 80, 0, 0});
    assert(!huge && huge.error().code == EOVERFLOW);
}
