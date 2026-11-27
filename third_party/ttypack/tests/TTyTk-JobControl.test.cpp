#include <TTyTk/TTyTk-JobControl.hpp>

#include <cassert>

namespace {

auto test_foreground() -> int {
    const auto controller = ::getpgrp();
    if (::tcgetpgrp(STDIN_FILENO) != controller) return 1;
    const auto child = ::fork();
    if (child < 0) return 2;
    if (child == 0) {
        if (::setpgid(0, 0) < 0) ::_exit(3);
        ::raise(SIGSTOP);
        ::_exit(::tcgetpgrp(STDIN_FILENO) == ::getpgrp() ? 0 : 4);
    }
    int status{};
    if (::waitpid(child, &status, WUNTRACED) != child || !WIFSTOPPED(status)) return 5;
    if (!ttytk::job::foreground(child)) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        return 6;
    }
    if (::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 7;
    if (!ttytk::job::foreground(controller) || ::tcgetpgrp(STDIN_FILENO) != controller) return 8;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return test_foreground();
    const auto invalid_resume = ttytk::job::resume(0);
    assert(!invalid_resume && invalid_resume.error().code == EINVAL);
    const auto invalid_suspend = ttytk::job::suspend(-1);
    assert(!invalid_suspend && invalid_suspend.error().code == EINVAL);
    const auto invalid_fg = ttytk::job::foreground(::getpid(), -1);
    assert(!invalid_fg && invalid_fg.error().code == EBADF);

    const auto child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        if (::setpgid(0, 0) < 0) ::_exit(2);
        ::raise(SIGSTOP);
        ::_exit(0);
    }
    int status{};
    assert(::waitpid(child, &status, WUNTRACED) == child);
    assert(WIFSTOPPED(status));
    assert(ttytk::job::background(child));
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    auto controller = ttytk::PTY::fork(argv[0], {"--test-foreground"});
    assert(controller);
    const auto waited = controller.value().wait();
    assert(waited && waited.value() && WIFEXITED(*waited.value()));
    assert(WEXITSTATUS(*waited.value()) == 0);
}
