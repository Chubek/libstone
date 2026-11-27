#include <TTyUtils/TTyUtils-Shell.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::shell;
    using namespace std::chrono_literals;
    auto completed = run({"/bin/sh", "-c", "printf 'out\\n'; printf tail >&2; exit 9"}, Capture::Both);
    std::vector<std::string> lines;
    completed.on_line([&](std::string line) { lines.push_back(std::move(line)); });
    assert(completed.wait() == 9);
    assert(!completed.running() && completed.wait() == 9);
    assert((lines == std::vector<std::string>{"out", "tail"}));

    auto pipeline = run(Pipeline{{{"printf", "abc\\n"}, {"tr", "a-z", "A-Z"}}}, {.capture = Capture::Stdout});
    lines.clear();
    pipeline.on_line([&](std::string line) { lines.push_back(std::move(line)); });
    assert(pipeline.wait() == 0);
    assert((lines == std::vector<std::string>{"ABC"}));

    auto timeout = run({"/bin/sh", "-c", "trap '' TERM; sleep 10"},
                       Options{.timeout = 150ms, .terminate_grace = 10ms});
    assert(timeout.wait() == 128 + SIGKILL);
    auto nonexistent = run({"/nonexistent/ttypack"});
    assert(nonexistent.wait() == 127);
}
