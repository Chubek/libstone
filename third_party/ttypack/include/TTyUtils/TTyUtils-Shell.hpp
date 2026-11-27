#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <initializer_list>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ttyutils::shell {

enum class Capture { None, Stdout, Stderr, Both };

struct Options {
    Capture capture{Capture::None};
    std::optional<std::chrono::milliseconds> timeout{};
    std::chrono::milliseconds terminate_grace{200};
};

struct Pipeline {
    std::vector<std::vector<std::string>> commands{};
};

namespace detail {

struct Pipe {
    int input{-1};
    int output{-1};

    Pipe() = default;
    Pipe(const Pipe&) = delete;
    auto operator=(const Pipe&) -> Pipe& = delete;
    ~Pipe() { close(input); close(output); }

    static void close(int& fd) noexcept {
        if (fd >= 0) (void)::close(std::exchange(fd, -1));
    }

    auto open() noexcept -> bool {
        int fds[2]{};
        if (::pipe(fds) != 0) return false;
        input = fds[0];
        output = fds[1];
        for (int* fd : {&input, &output}) {
            const int copy = ::fcntl(*fd, F_DUPFD_CLOEXEC, 3);
            if (copy < 0) return false;
            close(*fd);
            *fd = copy;
        }
        return true;
    }
};

}  // namespace detail

/// Owns child processes and drives capture until the pipeline has exited.
class Job {
public:
    Job() = default;
    explicit Job(const pid_t pid) { if (pid > 0) children_.push_back({pid}); }
    Job(const Job&) = delete;
    auto operator=(const Job&) -> Job& = delete;
    Job(Job&& other) noexcept
        : children_(std::exchange(other.children_, {})), group_(std::exchange(other.group_, -1)),
          capture_(std::exchange(other.capture_, -1)), callback_(std::move(other.callback_)),
          line_(std::move(other.line_)), deadline_(other.deadline_), kill_at_(other.kill_at_),
          terminate_grace_(other.terminate_grace_) {}
    auto operator=(Job&& other) noexcept -> Job& {
        if (this != &other) {
            Job previous(std::move(*this));
            children_ = std::exchange(other.children_, {});
            group_ = std::exchange(other.group_, -1);
            capture_ = std::exchange(other.capture_, -1);
            callback_ = std::move(other.callback_);
            line_ = std::move(other.line_);
            deadline_ = other.deadline_;
            kill_at_ = other.kill_at_;
            terminate_grace_ = other.terminate_grace_;
        }
        return *this;
    }
    ~Job() {
        reap();
        signal(SIGKILL);
        for (auto& child : children_) {
            if (!child.status) {
                int status = 0;
                while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
        detail::Pipe::close(capture_);
    }

    /// Return the last stage's exit code, or 128 plus its terminating signal.
    [[nodiscard]] auto wait() -> int {
        while (poll()) {
            ::pollfd descriptor{capture_, POLLIN, 0};
            (void)::poll(&descriptor, 1, 20);
        }
        return children_.empty() ? -1 : children_.back().status.value_or(-1);
    }

    /// Drain available captured lines, reap exited children, and check deadlines.
    [[nodiscard]] auto poll() -> bool {
        const auto now = Clock::now();
        reap();
        if (deadline_ && now >= *deadline_) terminate();
        if (kill_at_ && now >= *kill_at_) { signal(SIGKILL); kill_at_.reset(); }
        // Bound each drain so a continuously writing child cannot starve timeouts.
        for (unsigned reads = 0; capture_ >= 0 && reads < 64; ++reads) {
            char buffer[4096];
            const auto count = ::read(capture_, buffer, sizeof(buffer));
            if (count > 0) {
                if (callback_) for (ssize_t i = 0; i < count; ++i) {
                    if (buffer[i] == '\n') callback_(std::exchange(line_, {}));
                    else line_ += buffer[i];
                }
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count == 0 || !unreaped() || (errno != EAGAIN && errno != EWOULDBLOCK)) finish_capture();
            break;
        }
        return unreaped() || capture_ >= 0;
    }

    /// Request TERM immediately and KILL after the configured grace period.
    void terminate() noexcept {
        reap();
        deadline_.reset();
        if (unreaped() && !kill_at_) {
            signal(SIGTERM);
            kill_at_ = Clock::now() + terminate_grace_;
        }
    }
    void suspend() noexcept { reap(); signal(SIGSTOP); }
    void resume() noexcept { reap(); signal(SIGCONT); }
    void on_line(std::function<void(std::string)> callback) { callback_ = std::move(callback); }
    [[nodiscard]] auto running() const noexcept -> bool { reap(); return unreaped(); }

private:
    friend auto run(const Pipeline&, const Options&) -> Job;
    using Clock = std::chrono::steady_clock;
    struct Child { pid_t pid; std::optional<int> status{}; };

    void reap() const noexcept {
        for (auto& child : children_) {
            if (child.status) continue;
            int status = 0;
            pid_t result;
            do { result = ::waitpid(child.pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
            if (result < 0) child.status = -1;
            if (result > 0 && WIFEXITED(status)) child.status = WEXITSTATUS(status);
            if (result > 0 && WIFSIGNALED(status)) child.status = 128 + WTERMSIG(status);
        }
    }
    [[nodiscard]] auto unreaped() const noexcept -> bool {
        for (const auto& child : children_) if (!child.status) return true;
        return false;
    }
    void signal(const int value) const noexcept {
        if (!unreaped()) return;
        if (group_ > 0) (void)::kill(-group_, value);
        else for (const auto& child : children_) if (!child.status) (void)::kill(child.pid, value);
    }
    void finish_capture() {
        detail::Pipe::close(capture_);
        if (callback_ && !line_.empty()) callback_(std::exchange(line_, {}));
    }

    mutable std::vector<Child> children_;
    pid_t group_{-1};
    int capture_{-1};
    std::function<void(std::string)> callback_{};
    std::string line_;
    std::optional<Clock::time_point> deadline_;
    std::optional<Clock::time_point> kill_at_;
    std::chrono::milliseconds terminate_grace_{200};
};

class JobTable {
public:
    void add(Job job) { jobs_.push_back(std::move(job)); }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return jobs_.size(); }
    void suspend() noexcept { for (auto& job : jobs_) job.suspend(); }
    void resume() noexcept { for (auto& job : jobs_) job.resume(); }
    void terminate() noexcept { for (auto& job : jobs_) job.terminate(); }

private:
    std::vector<Job> jobs_;
};

/// Launch argv vectors directly, without shell interpolation.
inline auto run(const Pipeline& pipeline, const Options& options = {}) -> Job {
    const auto& stages = pipeline.commands;
    if (stages.empty()) return {};
    std::vector<std::vector<char*>> arguments;
    arguments.reserve(stages.size());
    for (const auto& stage : stages) {
        if (stage.empty() || stage.front().empty()) return {};
        auto& argv = arguments.emplace_back();
        for (const auto& arg : stage) {
            if (arg.find('\0') != std::string::npos) return {};
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
    }
    std::vector<detail::Pipe> pipes(stages.size() - 1);
    for (auto& pipe : pipes) if (!pipe.open()) return {};
    detail::Pipe captured;
    if (options.capture != Capture::None &&
        (!captured.open() || ::fcntl(captured.input, F_SETFL, O_NONBLOCK) != 0)) return {};

    Job job;
    job.children_.reserve(stages.size());
    job.terminate_grace_ = options.terminate_grace;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto pid = ::fork();
        if (pid < 0) return {};
        if (pid == 0) {
            if (::setpgid(0, job.group_ < 0 ? 0 : job.group_) != 0) _exit(126);
            if (i > 0 && ::dup2(pipes[i - 1].input, STDIN_FILENO) < 0) _exit(126);
            if (i + 1 < stages.size()) {
                if (::dup2(pipes[i].output, STDOUT_FILENO) < 0) _exit(126);
            } else if (options.capture == Capture::Stdout || options.capture == Capture::Both) {
                if (::dup2(captured.output, STDOUT_FILENO) < 0) _exit(126);
            }
            if (options.capture == Capture::Stderr || options.capture == Capture::Both) {
                if (::dup2(captured.output, STDERR_FILENO) < 0) _exit(126);
            }
            for (auto& pipe : pipes) {
                detail::Pipe::close(pipe.input);
                detail::Pipe::close(pipe.output);
            }
            detail::Pipe::close(captured.input);
            detail::Pipe::close(captured.output);
            ::execvp(arguments[i][0], arguments[i].data());
            _exit(errno == ENOENT ? 127 : 126);
        }
        if (job.group_ < 0) job.group_ = pid;
        (void)::setpgid(pid, job.group_);
        job.children_.push_back({pid});
    }
    if (options.timeout) job.deadline_ = Job::Clock::now() + *options.timeout;
    job.capture_ = std::exchange(captured.input, -1);
    return job;
}

inline auto run(const std::vector<std::string>& command, const Options& options) -> Job {
    return run(Pipeline{{command}}, options);
}
inline auto run(const std::vector<std::string>& command, const Capture capture = Capture::None) -> Job {
    return run(command, Options{.capture = capture});
}

}  // namespace ttyutils::shell
