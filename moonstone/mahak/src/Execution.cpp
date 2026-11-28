#include "mahak/Execution.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "mahak/Measurement.hpp"

extern char **environ;

namespace moonstone::mahak {
inline namespace v1 {
namespace {

struct Child {
    pid_t pid = -1;
    int to_stdin = -1;
    int from_stdout = -1;
};

void close_fd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

std::vector<std::string> environment_entries(const std::vector<EnvironmentVariable> &overrides,
                                             bool isolate) {
    std::vector<std::string> entries;
    if (!isolate) {
        for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry)
            entries.emplace_back(*entry);
    }
    for (const auto &variable : overrides) {
        const std::string assignment = variable.name + "=" + variable.value;
        bool replaced = false;
        for (auto &entry : entries) {
            const std::size_t separator = entry.find('=');
            const std::string_view name(entry.data(), separator);
            if (name != variable.name)
                continue;
            replaced = true;
            if (variable.overwrite)
                entry = assignment;
        }
        if (!replaced)
            entries.push_back(assignment);
    }
    return entries;
}

bool write_all(int fd, const std::string &data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

Result<PersistentResult> run_persistent(const CommandPlan &command,
                                        const PersistentOptions &options) {
    if (command.argv.empty()) {
        Error error;
        error.category = ErrorCategory::InvalidInput;
        error.message = "run-persistent: argv must not be empty";
        return error;
    }
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
        Error error;
        error.category = ErrorCategory::Internal;
        error.message = std::string("run-persistent: pipe: ") + std::strerror(errno);
        return error;
    }
    std::vector<std::string> environment =
        environment_entries(command.environment, options.process.isolate_environment);
    std::vector<char *> envp;
    for (auto &entry : environment)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<char *> argv;
    for (const auto &argument : command.argv)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        Error error;
        error.category = ErrorCategory::Internal;
        error.message = std::string("run-persistent: fork: ") + std::strerror(errno);
        return error;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        if (!command.working_directory.empty() && ::chdir(command.working_directory.c_str()) != 0)
            ::_exit(127);
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }
    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);

    PersistentResult result;
    CommandResult launch;
    launch.argv = command.argv;
    launch.validity = Validity::Valid;
    result.launch = launch;

    std::string pending;
    char buffer[4096];
    const std::uint32_t iterations =
        options.max_iterations == 0 ? 1 : options.max_iterations;
    const std::uint64_t iteration_timeout_ns =
        options.iteration_timeout_ms * 1000000ull;
    bool child_running = true;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        if (!child_running)
            break;
        const std::string line_input = options.iteration_input + "\n";
        const std::uint64_t started = monotonic_now_ns();
        if (!write_all(stdin_pipe[1], line_input)) {
            CommandResult iteration;
            iteration.argv = command.argv;
            iteration.validity = Validity::Crashed;
            iteration.message = "persistent child closed stdin";
            iteration.wall_ns = monotonic_now_ns() - started;
            result.iterations.push_back(std::move(iteration));
            break;
        }
        CommandResult iteration;
        iteration.argv = command.argv;
        std::string line;
        bool got_line = false;
        while (!got_line) {
            const std::size_t newline = pending.find('\n');
            if (newline != std::string::npos) {
                line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                got_line = true;
                break;
            }
            const std::uint64_t now = monotonic_now_ns();
            if (iteration_timeout_ns != 0 && now - started >= iteration_timeout_ns) {
                iteration.validity = Validity::TimedOut;
                iteration.message = "persistent iteration timed out";
                ::kill(-pid, SIGKILL);
                child_running = false;
                break;
            }
            const int wait_ms = iteration_timeout_ns == 0
                                    ? 100
                                    : static_cast<int>(
                                          std::min<std::uint64_t>(100, (iteration_timeout_ns - (now - started)) / 1000000ull + 1));
            pollfd descriptor{};
            descriptor.fd = stdout_pipe[0];
            descriptor.events = POLLIN;
            const int ready = ::poll(&descriptor, 1, wait_ms);
            if (ready < 0 && errno != EINTR) {
                iteration.validity = Validity::MeasurementFailed;
                iteration.message = std::string("poll failed: ") + std::strerror(errno);
                child_running = false;
                break;
            }
            if (ready <= 0) {
                if (options.process.cancel != nullptr && options.process.cancel->load()) {
                    iteration.validity = Validity::Cancelled;
                    iteration.message = "cancelled by the harness";
                    ::kill(-pid, SIGKILL);
                    child_running = false;
                    break;
                }
                continue;
            }
            const ssize_t bytes = ::read(stdout_pipe[0], buffer, sizeof(buffer));
            if (bytes > 0) {
                pending.append(buffer, static_cast<std::size_t>(bytes));
                continue;
            }
            if (bytes == 0) {
                int status = 0;
                ::waitpid(pid, &status, 0);
                child_running = false;
                iteration.validity = Validity::Crashed;
                iteration.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                iteration.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
                iteration.message = "persistent child exited before producing a line";
                break;
            }
            if (errno != EAGAIN && errno != EINTR) {
                iteration.validity = Validity::MeasurementFailed;
                iteration.message = std::string("read failed: ") + std::strerror(errno);
                child_running = false;
                break;
            }
        }
        iteration.wall_ns = monotonic_now_ns() - started;
        iteration.stdout_text = line;
        if (got_line) {
            iteration.exit_code = 0;
            iteration.validity = Validity::Valid;
        }
        result.iterations.push_back(std::move(iteration));
    }

    close_fd(stdin_pipe[1]);
    close_fd(stdout_pipe[0]);
    if (child_running) {
        ::kill(-pid, SIGTERM);
        int status = 0;
        bool reaped = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            const pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                reaped = true;
                break;
            }
            ::usleep(5000);
        }
        if (!reaped) {
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
        result.note = "persistent child terminated by the harness after the final iteration";
    }
    return result;
}

} // namespace v1
} // namespace moonstone::mahak
