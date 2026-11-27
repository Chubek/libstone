#include <TTyTk/TTyTk-MuxTools.hpp>
#include <cassert>
#include <poll.h>

int main() {
    ttytk::Mux mux;
    assert(mux.next_window() == nullptr);
    assert(!mux.send_input("nothing"));
    const auto root = mux.add_window();
    mux.active_window()->screen.resize(1, 2);
    mux.active_window()->screen.put(0, 0, ttytk::Cell{"R"});
    const auto split = mux.split(ttytk::Window::Vertical);
    const auto info = mux.split_info(split);
    assert(!mux.split_info(root));
    assert(info && info->parent == root && info->direction == ttytk::Window::Vertical);
    assert(mux.next_window()->screen.at(0, 0).grapheme == "R");
    assert(mux.next_window()->screen.rows() == 0);
    const auto missing = mux.receive_output([](auto&, auto) { assert(false); });
    assert(!missing && missing.error().code == EBADF);

    auto pty = ttytk::PTY::fork("sh", {"-c", "IFS= read -r line; printf 'got:%s\\n' \"$line\""});
    if (!pty) return 1;
    mux.active_window()->terminal = std::move(pty).value();
    mux.active_window()->screen.resize(1, 1);
    auto* pane = mux.active_window();
    const auto sent = mux.send_input("hello\n");
    if (!sent || sent.value() != 6) return 1;
    std::string transcript;
    while (transcript.find("got:hello") == std::string::npos) {
        pollfd watched{pane->terminal.master_fd(), POLLIN, 0};
        if (::poll(&watched, 1, 3000) != 1) return 1;
        const auto received = mux.receive_output([&](auto& screen, auto bytes) {
            assert(&screen == &pane->screen);
            transcript += bytes;
            if (transcript.find("got:hello") != std::string::npos) screen.put(0, 0, ttytk::Cell{"G"});
        });
        if (!received || received.value() == 0) return 1;
    }
    const auto waited = pane->terminal.wait();
    assert(waited && waited.value() && WIFEXITED(*waited.value()));
    assert(pane->screen.at(0, 0).grapheme == "G");
}
