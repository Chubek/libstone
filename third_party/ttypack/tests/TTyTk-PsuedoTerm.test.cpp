#include <TTyTk/TTyTk-PsuedoTerm.hpp>

#include <cassert>
#include <string>

int main() {
    ttytk::PTY pty{};
    assert(pty.master_fd() == -1);
    assert(!pty.resize({24, 80, 0, 0}));
    auto missing = ttytk::PTY::fork("/no/such/ttypack-test-command", {});
    assert(!missing && missing.error().code == ENOENT);
    auto launched = ttytk::PTY::fork("sh", {"-c", "test -t 0 && test -t 1 || exit 3; "
                                                 "IFS= read -r line; printf 'got:%s\\n' \"$line\""});
    assert(launched);
    pty = std::move(launched.value());
    assert(pty.child_pid() > 1);
    assert(pty.resize({24, 80, 640, 480}));
    const auto dimensions = ttytk::winsize::get(pty.master_fd());
    assert(dimensions && dimensions.value().rows == 24 && dimensions.value().columns == 80);
    assert(dimensions.value().pixel_width == 640 && dimensions.value().pixel_height == 480);
    assert(pty.write("hello\n").value() == 6);
    std::string output;
    char buffer[64]{};
    for (;;) {
        const auto count = pty.read(buffer, sizeof(buffer));
        assert(count);
        if (count.value() == 0) break;
        output.append(buffer, count.value());
    }
    assert(output.find("got:hello") != std::string::npos);
    const auto waited = pty.wait();
    assert(waited && waited.value() && WIFEXITED(*waited.value()));
    assert(WEXITSTATUS(*waited.value()) == 0);
    assert(pty.child_pid() == -1);
}
