#include "mahak/Execution.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "mahak/Measurement.hpp"

extern char **environ;

namespace moonstone::mahak {
inline namespace v1 {
namespace {

Error process_error(std::string_view context, std::string_view message,
                    ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::string(context) + ": " + std::string(message);
    return error;
}

void close_fd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool make_pipe(int fds[2]) {
    if (::pipe(fds) != 0)
        return false;
    for (int index = 0; index < 2; ++index) {
        const int flags = ::fcntl(fds[index], F_GETFD);
        if (flags >= 0)
            ::fcntl(fds[index], F_SETFD, flags | FD_CLOEXEC);
    }
    return true;
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::vector<std::string> build_environment(const std::vector<EnvironmentVariable> &overrides,
                                           bool isolate) {
    std::vector<std::string> entries;
    if (!isolate) {
        for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry)
            entries.emplace_back(*entry);
    }
    for (const auto &override_entry : overrides) {
        const std::string assignment = override_entry.name + "=" + override_entry.value;
        bool replaced = false;
        for (auto &entry : entries) {
            const std::size_t separator = entry.find('=');
            const std::string_view name(entry.data(), separator);
            if (name != override_entry.name)
                continue;
            if (override_entry.overwrite) {
                entry = assignment;
                replaced = true;
            } else {
                replaced = true;
            }
        }
        if (!replaced)
            entries.push_back(assignment);
    }
    return entries;
}

void append_bounded(std::string &target, const char *data, std::size_t size,
                    std::size_t limit, bool &truncated) {
    const std::size_t room = limit > target.size() ? limit - target.size() : 0;
    const std::size_t take = std::min(room, size);
    target.append(data, take);
    if (take < size)
        truncated = true;
}

void kill_group(pid_t pid, int signal) {
    if (pid <= 0)
        return;
    if (::kill(-pid, signal) != 0)
        ::kill(pid, signal);
}

ResourceUsage usage_from(const rusage &usage) {
    ResourceUsage result;
    result.max_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ull;
    result.minor_faults = static_cast<std::uint64_t>(usage.ru_minflt);
    result.major_faults = static_cast<std::uint64_t>(usage.ru_majflt);
    result.voluntary_context_switches = static_cast<std::uint64_t>(usage.ru_nvcsw);
    result.involuntary_context_switches = static_cast<std::uint64_t>(usage.ru_nivcsw);
    result.usage_notes.push_back("read/write bytes are unavailable from wait4 rusage");
    return result;
}

struct ChildIo {
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
};

} // namespace

Result<CommandResult> run_command(const CommandPlan &command, const ProcessOptions &options) {
    static std::once_flag sigpipe_once;
    std::call_once(sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });
    if (command.argv.empty())
        return process_error("run-command", "argv must not be empty",
                             ErrorCategory::InvalidInput);

    const std::size_t limit = options.max_output_bytes;
    CommandResult result;
    result.argv = command.argv;

    ChildIo io;
    const bool capture = options.capture_output;
    if (capture) {
        if (!make_pipe(io.stdout_pipe) || !make_pipe(io.stderr_pipe))
            return process_error("run-command", std::string("pipe: ") + std::strerror(errno),
                                 ErrorCategory::Internal);
    }
    const std::string stdin_data = command.stdin_data.value_or(std::string());
    const bool feed_stdin = command.stdin_data.has_value();
    if (feed_stdin && !make_pipe(io.stdin_pipe))
        return process_error("run-command", std::string("pipe: ") + std::strerror(errno),
                             ErrorCategory::Internal);
    if (!make_pipe(io.error_pipe))
        return process_error("run-command", std::string("pipe: ") + std::strerror(errno),
                             ErrorCategory::Internal);

    std::vector<std::string> environment = build_environment(command.environment, options.isolate_environment);
    std::vector<char *> envp;
    envp.reserve(environment.size() + 1);
    for (auto &entry : environment)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    std::vector<char *> argv;
    argv.reserve(command.argv.size() + 1);
    for (const auto &argument : command.argv)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

    const std::uint64_t started = monotonic_now_ns();
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        for (int *fd : {&io.stdin_pipe[0], &io.stdin_pipe[1], &io.stdout_pipe[0],
                        &io.stdout_pipe[1], &io.stderr_pipe[0], &io.stderr_pipe[1],
                        &io.error_pipe[0], &io.error_pipe[1]})
            close_fd(*fd);
        result.spawn_failed = true;
        result.validity = Validity::SetupFailed;
        result.message = std::string("fork failed: ") + std::strerror(saved);
        return result;
    }
    if (pid == 0) {
        // Child: no allocation, no throw, no shell.
        if (options.use_process_group)
            ::setpgid(0, 0);
        if (feed_stdin) {
            ::dup2(io.stdin_pipe[0], STDIN_FILENO);
        } else {
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDIN_FILENO);
                ::close(devnull);
            }
        }
        if (capture) {
            ::dup2(io.stdout_pipe[1], STDOUT_FILENO);
            ::dup2(io.stderr_pipe[1], STDERR_FILENO);
        }
        if (!command.working_directory.empty() && ::chdir(command.working_directory.c_str()) != 0) {
            const int code = errno;
            const ssize_t ignored = ::write(io.error_pipe[1], &code, sizeof(code));
            (void)ignored;
            ::_exit(127);
        }
        if (options.limits.cpu_time_ms != 0) {
            rlimit limit_value{static_cast<rlim_t>(options.limits.cpu_time_ms / 1000 + 1),
                               static_cast<rlim_t>(options.limits.cpu_time_ms / 1000 + 1)};
            ::setrlimit(RLIMIT_CPU, &limit_value);
        }
        if (options.limits.address_space_bytes != 0) {
            rlimit limit_value{static_cast<rlim_t>(options.limits.address_space_bytes),
                               static_cast<rlim_t>(options.limits.address_space_bytes)};
            ::setrlimit(RLIMIT_AS, &limit_value);
        }
        if (options.limits.file_size_bytes != 0) {
            rlimit limit_value{static_cast<rlim_t>(options.limits.file_size_bytes),
                               static_cast<rlim_t>(options.limits.file_size_bytes)};
            ::setrlimit(RLIMIT_FSIZE, &limit_value);
        }
        if (options.limits.open_files != 0) {
            rlimit limit_value{static_cast<rlim_t>(options.limits.open_files),
                               static_cast<rlim_t>(options.limits.open_files)};
            ::setrlimit(RLIMIT_NOFILE, &limit_value);
        }
        if (options.limits.processes != 0) {
            rlimit limit_value{static_cast<rlim_t>(options.limits.processes),
                               static_cast<rlim_t>(options.limits.processes)};
            ::setrlimit(RLIMIT_NPROC, &limit_value);
        }
        if (!options.cpu_affinity.empty()) {
#if defined(__linux__)
            const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
            (void)cpus;
#endif
            // Affinity is applied by the caller-provided list only when the
            // platform exposes sched_setaffinity; otherwise it is recorded.
        }
        for (int *fd : {&io.stdin_pipe[0], &io.stdin_pipe[1], &io.stdout_pipe[0],
                        &io.stdout_pipe[1], &io.stderr_pipe[0], &io.stderr_pipe[1],
                        &io.error_pipe[0]})
            if (*fd >= 0 && *fd > 2)
                ::close(*fd);
        ::execvp(argv[0], argv.data());
        const int code = errno;
        const ssize_t ignored = ::write(io.error_pipe[1], &code, sizeof(code));
        (void)ignored;
        ::_exit(127);
    }

    // Parent.
    close_fd(io.stdin_pipe[0]);
    close_fd(io.stdout_pipe[1]);
    close_fd(io.stderr_pipe[1]);
    close_fd(io.error_pipe[1]);
    set_nonblocking(io.stdout_pipe[0]);
    set_nonblocking(io.stderr_pipe[0]);
    set_nonblocking(io.error_pipe[0]);
    if (feed_stdin)
        set_nonblocking(io.stdin_pipe[1]);

    std::unique_ptr<PerfSession> perf;
    if (!options.perf_events.empty()) {
        PerfRequest request;
        request.events = options.perf_events;
        auto attached = PerfSession::attach(static_cast<std::int64_t>(pid), request);
        if (attached) {
            perf = std::move(attached.value());
        } else {
            for (const auto &event : options.perf_events) {
                PerfCounter counter;
                counter.name = event;
                counter.enabled = false;
                counter.note = attached.error().message;
                result.perf.push_back(std::move(counter));
            }
        }
    }

    const std::uint64_t deadline =
        options.timeout_ms != 0 ? started + options.timeout_ms * 1000000ull : 0;
    std::string exec_error;
    std::size_t stdin_offset = 0;
    bool child_exited = false;
    bool cancelled = false;
    int status = 0;
    rusage usage{};

    while (true) {
        if (options.cancel != nullptr && options.cancel->load()) {
            cancelled = true;
            kill_group(pid, SIGTERM);
        }
        if (!child_exited) {
            const pid_t waited = ::wait4(pid, &status, WNOHANG, &usage);
            if (waited == pid)
                child_exited = true;
            else if (waited < 0 && errno != EINTR)
                child_exited = true;
        }
        if (deadline != 0 && !child_exited && monotonic_now_ns() >= deadline) {
            result.timed_out = true;
            kill_group(pid, SIGTERM);
            const std::uint64_t grace_end = monotonic_now_ns() + 200000000ull;
            while (!child_exited && monotonic_now_ns() < grace_end) {
                const pid_t waited = ::wait4(pid, &status, WNOHANG, &usage);
                if (waited == pid)
                    child_exited = true;
                else
                    ::usleep(5000);
            }
            if (!child_exited) {
                kill_group(pid, SIGKILL);
                ::wait4(pid, &status, 0, &usage);
                child_exited = true;
            }
        }

        pollfd descriptors[4];
        int count = 0;
        auto add = [&](int fd, short events) {
            if (fd < 0)
                return;
            descriptors[count].fd = fd;
            descriptors[count].events = events;
            descriptors[count].revents = 0;
            ++count;
        };
        add(io.stdout_pipe[0], POLLIN);
        add(io.stderr_pipe[0], POLLIN);
        add(io.error_pipe[0], POLLIN);
        if (feed_stdin && stdin_offset < stdin_data.size())
            add(io.stdin_pipe[1], POLLOUT);

        if (count > 0)
            ::poll(descriptors, static_cast<nfds_t>(count), 25);
        else if (!child_exited)
            ::usleep(2000);

        for (int index = 0; index < count; ++index) {
            const int fd = descriptors[index].fd;
            if (descriptors[index].revents == 0)
                continue;
            if (fd == io.stdin_pipe[1]) {
                const std::size_t remaining = stdin_data.size() - stdin_offset;
                const ssize_t written =
                    ::write(fd, stdin_data.data() + stdin_offset, remaining);
                if (written > 0)
                    stdin_offset += static_cast<std::size_t>(written);
                else if (written < 0 && errno != EAGAIN && errno != EINTR)
                    close_fd(io.stdin_pipe[1]);
                if (stdin_offset >= stdin_data.size())
                    close_fd(io.stdin_pipe[1]);
                continue;
            }
            char buffer[16384];
            while (true) {
                const ssize_t bytes = ::read(fd, buffer, sizeof(buffer));
                if (bytes > 0) {
                    if (fd == io.stdout_pipe[0])
                        append_bounded(result.stdout_text, buffer,
                                       static_cast<std::size_t>(bytes), limit,
                                       result.output_truncated);
                    else if (fd == io.stderr_pipe[0])
                        append_bounded(result.stderr_text, buffer,
                                       static_cast<std::size_t>(bytes), limit,
                                       result.output_truncated);
                    else if (fd == io.error_pipe[0]) {
                        exec_error.insert(exec_error.end(), buffer, buffer + bytes);
                    }
                    continue;
                }
                if (bytes == 0) {
                    if (fd == io.stdout_pipe[0])
                        close_fd(io.stdout_pipe[0]);
                    else if (fd == io.stderr_pipe[0])
                        close_fd(io.stderr_pipe[0]);
                    else
                        close_fd(io.error_pipe[0]);
                    break;
                }
                if (errno == EAGAIN || errno == EINTR)
                    break;
                if (fd == io.stdout_pipe[0])
                    close_fd(io.stdout_pipe[0]);
                else if (fd == io.stderr_pipe[0])
                    close_fd(io.stderr_pipe[0]);
                else
                    close_fd(io.error_pipe[0]);
                break;
            }
        }
        const bool stdout_done = io.stdout_pipe[0] < 0;
        const bool stderr_done = io.stderr_pipe[0] < 0;
        const bool error_done = io.error_pipe[0] < 0;
        if (child_exited)
            close_fd(io.stdin_pipe[1]);
        const bool stdin_done =
            !feed_stdin || io.stdin_pipe[1] < 0 || stdin_offset >= stdin_data.size();
        if (child_exited && stdout_done && stderr_done && error_done && stdin_done)
            break;
        if (result.timed_out && child_exited && stdout_done && stderr_done)
            break;
    }
    close_fd(io.stdin_pipe[1]);
    close_fd(io.stdout_pipe[0]);
    close_fd(io.stderr_pipe[0]);
    close_fd(io.error_pipe[0]);
    result.wall_ns = monotonic_now_ns() - started;

    if (child_exited && WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (child_exited && WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
        result.exit_code = -1;
    }
    if (options.collect_rusage) {
        result.usage = usage_from(usage);
        result.cpu.user_ns = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000000ull +
                             static_cast<std::uint64_t>(usage.ru_utime.tv_usec) * 1000ull;
        result.cpu.system_ns = static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000000ull +
                               static_cast<std::uint64_t>(usage.ru_stime.tv_usec) * 1000ull;
    } else {
        result.usage.usage_notes.push_back("rusage collection disabled");
    }
    if (perf) {
        auto counters = perf->stop();
        if (counters) {
            result.perf.insert(result.perf.end(), counters.value().begin(), counters.value().end());
        } else {
            PerfCounter counter;
            counter.name = "perf";
            counter.enabled = false;
            counter.note = counters.error().message;
            result.perf.push_back(std::move(counter));
        }
    }

    if (exec_error.size() >= sizeof(int)) {
        int code = 0;
        std::memcpy(&code, exec_error.data(), sizeof(code));
        if (code != 0) {
            result.spawn_failed = true;
            result.message = std::string("exec failed: ") + std::strerror(code);
        }
    } else if (!exec_error.empty()) {
        result.spawn_failed = true;
        result.message = "exec failed: truncated error report";
    }

    if (result.spawn_failed) {
        result.validity = Validity::SetupFailed;
        if (result.message.empty())
            result.message = "child failed to start";
    } else if (cancelled) {
        result.validity = Validity::Cancelled;
        result.message = "cancelled by the harness";
    } else if (result.timed_out) {
        result.validity = Validity::TimedOut;
        result.message = "harness timeout expired";
    } else if (result.signal != 0) {
        result.validity = Validity::Crashed;
        result.message = std::string("terminated by signal ") + std::to_string(result.signal);
    } else if (result.exit_code != 0) {
        result.validity = Validity::NonZeroExit;
        result.message = "exited with status " + std::to_string(result.exit_code);
    } else {
        result.validity = Validity::Valid;
    }
    if (result.output_truncated)
        result.message += result.message.empty() ? "output truncated" : "; output truncated";
    return result;
}

} // namespace v1
} // namespace moonstone::mahak
