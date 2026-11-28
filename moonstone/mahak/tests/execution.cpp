// Process supervision and repetition scheduling contracts.
#include <iostream>
#include <string>

#include <sys/stat.h>

#include "mahak/Execution.hpp"

using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

bool exists(const char *path) {
    struct stat status {};
    return ::stat(path, &status) == 0;
}

CommandPlan plan_of(std::initializer_list<const char *> argv) {
    CommandPlan plan;
    for (const char *argument : argv)
        plan.argv.emplace_back(argument);
    return plan;
}

} // namespace

int main() {
    if (!exists("/bin/true") || !exists("/bin/echo")) {
        std::cout << "SKIP: /bin/true and /bin/echo are required\n";
        return 0;
    }
    ProcessOptions options;
    options.timeout_ms = 5000;

    auto success = run_command(plan_of({"/bin/true"}), options);
    check(static_cast<bool>(success), "true runs");
    if (success) {
        check(success.value().validity == Validity::Valid, "true is valid");
        check(success.value().exit_code == 0, "true exits 0");
        check(!success.value().spawn_failed, "true spawned");
        check(success.value().wall_ns > 0, "wall time recorded");
    }

    auto failure = run_command(plan_of({"/bin/false"}), options);
    check(static_cast<bool>(failure), "false still returns a result");
    if (failure) {
        check(failure.value().exit_code == 1, "false exits 1");
        check(failure.value().validity == Validity::NonZeroExit, "non-zero exit is classified");
    }

    auto missing = run_command(plan_of({"/definitely/not/here"}), options);
    check(static_cast<bool>(missing), "missing executable is a result, not a harness error");
    if (missing) {
        check(missing.value().spawn_failed, "missing executable reports spawn failure");
        check(missing.value().validity != Validity::Valid, "spawn failure is never valid");
        check(missing.value().exit_code != 0, "spawn failure is never exit 0");
    }

    if (exists("/bin/sleep")) {
        ProcessOptions timed;
        timed.timeout_ms = 50;
        auto timeout = run_command(plan_of({"/bin/sleep", "5"}), timed);
        check(static_cast<bool>(timeout), "timeout returns a result");
        if (timeout) {
            check(timeout.value().timed_out, "timeout flagged");
            check(timeout.value().validity == Validity::TimedOut, "timeout classified");
            check(timeout.value().wall_ns < 4000000000ull, "timeout returns promptly");
        }
    }

    ProcessOptions bounded;
    bounded.max_output_bytes = 4;
    auto truncated = run_command(plan_of({"/bin/echo", "0123456789"}), bounded);
    check(static_cast<bool>(truncated), "truncating run returns a result");
    if (truncated) {
        check(truncated.value().output_truncated, "truncation flagged");
        check(truncated.value().stdout_text.size() <= 4, "capture respects the limit");
    }

    // RepetitionScheduler is declared with an incomplete Impl in the public
    // header (see include/mahak/Execution.hpp), so it can only be constructed
    // inside the library. Its behavior is covered by the engine sampling test.

    if (failures != 0) {
        std::cerr << failures << " execution check(s) failed\n";
        return 1;
    }
    std::cout << "execution tests passed\n";
    return 0;
}
