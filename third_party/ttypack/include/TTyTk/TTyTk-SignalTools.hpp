#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <utility>

namespace ttytk::signals {

namespace detail {

using Callback = std::shared_ptr<std::function<void()>>;

static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
inline constinit std::atomic<int> wake_fd{-1};
inline constinit std::atomic<bool> winch_pending{false};
inline constinit std::atomic<bool> int_pending{false};

struct State {
    std::mutex mutex{};
    int read_fd{-1};
    struct sigaction old_winch {};
    struct sigaction old_int {};
    Callback winch{};
    Callback resize{};
    Callback interrupt{};
};

inline auto state() -> State& {
    static State value{};
    return value;
}

inline void handle_signal(const int number) noexcept {
    const int saved_errno = errno;
    auto& pending = number == SIGWINCH ? winch_pending : int_pending;
    pending.store(true);
    const int fd = wake_fd.load();
    if (fd >= 0) {
        const char marker = 0;
        while (::write(fd, &marker, 1) < 0 && errno == EINTR) {}
    }
    errno = saved_errno;
}

inline auto make_pipe(State& value) -> Result<void> {
    if (value.read_fd >= 0) return {};
    int fds[2]{};
#if defined(TTYTK_PLATFORM_LINUX)
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        return Error{errno, std::strerror(errno)};
    }
#else
    if (::pipe(fds) < 0) return Error{errno, std::strerror(errno)};
    for (const auto fd : fds) {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
            ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            const int error = errno;
            ::close(fds[0]);
            ::close(fds[1]);
            return Error{error, std::strerror(error)};
        }
    }
#endif
    // Keep both descriptors valid for the process lifetime: a handler on
    // another thread may still be writing during unregistration.
    value.read_fd = fds[0];
    wake_fd.store(fds[1]);
    return {};
}

inline auto subscribe(const int number, const bool resize,
                      std::function<void()> callback) -> Result<void> {
    Callback next{};
    if (callback) {
        next = std::make_shared<std::function<void()>>(std::move(callback));
    }
    auto& value = state();
    Callback replaced{};
    {
        std::lock_guard lock{value.mutex};
        auto& slot = number == SIGWINCH ?
            (resize ? value.resize : value.winch) : value.interrupt;
        auto& old_action = number == SIGWINCH ? value.old_winch : value.old_int;
        auto& pending = number == SIGWINCH ? winch_pending : int_pending;
        const bool other = number == SIGWINCH &&
            static_cast<bool>(resize ? value.winch : value.resize);
        if (!other && !slot && next) {
            auto pipe = make_pipe(value);
            if (!pipe) return pipe;
            struct sigaction action {};
            action.sa_handler = handle_signal;
            action.sa_flags = SA_RESTART;
            ::sigemptyset(&action.sa_mask);
            pending.store(false);
            if (::sigaction(number, &action, &old_action) < 0) {
                return Error{errno, std::strerror(errno)};
            }
        } else if (!other && slot && !next) {
            if (::sigaction(number, &old_action, nullptr) < 0) {
                return Error{errno, std::strerror(errno)};
            }
            pending.store(false);
        }
        replaced = std::exchange(slot, std::move(next));
    }
    return {};
}

}  // namespace detail

/** Register or replace a deferred SIGWINCH callback; pass empty to remove it. */
inline auto on_sigwinch(std::function<void()> callback) -> Result<void> {
    return detail::subscribe(SIGWINCH, false, std::move(callback));
}

/** Register or replace a deferred SIGINT callback; pass empty to remove it. */
inline auto on_sigint(std::function<void()> callback) -> Result<void> {
    return detail::subscribe(SIGINT, false, std::move(callback));
}

/** Borrow the nonblocking poll descriptor, or -1 before first registration. */
[[nodiscard]] inline auto notification_fd() -> int {
    auto& value = detail::state();
    std::lock_guard lock{value.mutex};
    return value.read_fd;
}

/**
 * Dispatch pending callbacks in the calling thread, not in the signal handler.
 * Poll notification_fd() for readability before calling, or call periodically.
 * Repeated deliveries of the same signal may be coalesced.
 */
inline void dispatch_pending() {
    auto& value = detail::state();
    detail::Callback winch{}, resize{}, interrupt{};
    {
        std::lock_guard lock{value.mutex};
        if (value.read_fd < 0) return;
        // Bound the drain so a concurrent stream of signals cannot starve
        // dispatch. Any remaining bytes simply keep the descriptor readable.
        char markers[256]{};
        ssize_t count;
        do { count = ::read(value.read_fd, markers, sizeof(markers)); }
        while (count < 0 && errno == EINTR);
        if (detail::winch_pending.exchange(false)) {
            winch = value.winch;
            resize = value.resize;
        }
        if (detail::int_pending.exchange(false)) interrupt = value.interrupt;
    }
    if (winch) (*winch)();
    if (resize) (*resize)();
    if (interrupt) (*interrupt)();
}

/** Block blockable signals in the calling thread. */
[[nodiscard]] inline auto block_all() -> Result<void> {
    sigset_t set{};
    if (::sigfillset(&set) != 0) return Error{errno, std::strerror(errno)};
    const int error = ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (error != 0) return Error{error, std::strerror(error)};
    return {};
}

}  // namespace ttytk::signals
