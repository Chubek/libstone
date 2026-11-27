#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-PsuedoTerm.hpp>
#include <TTyTk/TTyTk-SignalTools.hpp>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

namespace ttytk::job {

namespace detail {

inline auto prepare_group(const pid_t process) -> Result<void> {
    if (process <= 0) return Error{EINVAL, "job PID must be positive"};
    const auto existing = ::getpgid(process);
    if (existing < 0) return Error{errno, std::strerror(errno)};
    if (existing == process) return {};
    if (::setpgid(process, process) < 0) {
        const int error = errno;
        // The child may have established its own group concurrently.
        if (::getpgid(process) != process) return Error{error, std::strerror(error)};
    }
    return {};
}

inline auto signal_group(const pid_t group, const int number) -> Result<void> {
    // kill(-1, ...) is a broadcast, not a signal to group one.
    if (group <= 1) return Error{EINVAL, "job process group must exceed one"};
    if (::kill(-group, number) < 0) return Error{errno, std::strerror(errno)};
    return {};
}

inline auto give_terminal(const int fd, const pid_t group) -> Result<void> {
    sigset_t mask{}, previous{};
    ::sigemptyset(&mask);
    ::sigaddset(&mask, SIGTTOU);
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &mask, &previous);
    if (mask_error != 0) return Error{mask_error, std::strerror(mask_error)};
    int result;
    do { result = ::tcsetpgrp(fd, group); } while (result < 0 && errno == EINTR);
    const int error = errno;
    const int restore_error = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (result < 0) return Error{error, std::strerror(error)};
    if (restore_error != 0) return Error{restore_error, std::strerror(restore_error)};
    return {};
}

}  // namespace detail

/** Suspend a single process; zero and negative kill selectors are rejected. */
[[nodiscard]] inline auto suspend(const int pid) -> Result<void> {
    if (pid <= 0) return Error{EINVAL, "job PID must be positive"};
    if (::kill(static_cast<pid_t>(pid), SIGSTOP) != 0) {
        return Error{errno, std::strerror(errno)};
    }
    return {};
}

/** Resume a single process; zero and negative kill selectors are rejected. */
[[nodiscard]] inline auto resume(const int pid) -> Result<void> {
    if (pid <= 0) return Error{EINVAL, "job PID must be positive"};
    if (::kill(static_cast<pid_t>(pid), SIGCONT) != 0) {
        return Error{errno, std::strerror(errno)};
    }
    return {};
}

/**
 * Give a controlling terminal to the job group, then resume that group.
 * The caller retains responsibility for managing terminal ownership after wait.
 */
[[nodiscard]] inline auto foreground(const int pid, const int terminal_fd = STDIN_FILENO)
    -> Result<void> {
    if (pid <= 1) return Error{EINVAL, "job PID must exceed one"};
    const auto process = static_cast<pid_t>(pid);
    const auto previous = ::tcgetpgrp(terminal_fd);
    if (previous < 0) return Error{errno, std::strerror(errno)};
    auto prepared = detail::prepare_group(process);
    if (!prepared) return prepared;
    auto transferred = detail::give_terminal(terminal_fd, process);
    if (!transferred) return transferred;
    auto continued = detail::signal_group(process, SIGCONT);
    if (!continued && previous != process) {
        static_cast<void>(detail::give_terminal(terminal_fd, previous));
    }
    return continued;
}

/** Resume a job group without granting it the terminal. */
[[nodiscard]] inline auto background(const int pid) -> Result<void> {
    if (pid <= 1) return Error{EINVAL, "job PID must exceed one"};
    const auto process = static_cast<pid_t>(pid);
    auto prepared = detail::prepare_group(process);
    if (!prepared) return prepared;
    return detail::signal_group(process, SIGCONT);
}

}  // namespace ttytk::job
