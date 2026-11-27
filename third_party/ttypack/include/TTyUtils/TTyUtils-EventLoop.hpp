#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#else
#error "not implemented on this platform"
#endif

namespace ttyutils::loop {

enum EventMask : short { Readable = POLLIN, Writable = POLLOUT };

struct Event {
    enum class Kind { readable, writable, timer, idle, resize, signal };
    Kind kind{Kind::idle};
    int fd{-1};
    int signal_number{};
    short ready{};
};

class Loop;

namespace detail {

/// Copyable cancellation token for a loop registration. Destruction does not cancel it.
class Registration {
public:
    Registration() = default;
    void cancel() noexcept {
        if (const auto state = state_.lock()) state->store(false);
    }
    [[nodiscard]] auto active() const noexcept -> bool {
        const auto state = state_.lock();
        return state && state->load();
    }

protected:
    explicit Registration(const std::shared_ptr<std::atomic<bool>>& state) : state_(state) {}

private:
    std::weak_ptr<std::atomic<bool>> state_;
};

}  // namespace detail

class Timer : public detail::Registration {
private:
    friend class Loop;
    using Registration::Registration;
};

class Idle : public detail::Registration {
private:
    friend class Loop;
    using Registration::Registration;
};

class FdWatch : public detail::Registration {
public:
    /// Updates the interests on the loop thread; applied on the next poll.
    void set_events(const short events) noexcept {
        if (const auto interests = interests_.lock()) *interests = events;
    }
private:
    friend class Loop;
    FdWatch(const std::shared_ptr<std::atomic<bool>>& state, const std::shared_ptr<short>& interests)
        : Registration(state), interests_(interests) {}
    std::weak_ptr<short> interests_;
public:
    FdWatch() = default;
};

/// Dispatches callbacks on one thread. Only enqueue(), stop(), and cancellation
/// may be called from other threads, and the loop must outlive those callers.
class Loop {
public:
    using Callback = std::function<void(const Event&)>;
    using Task = std::function<void()>;

    Loop() {
        if (::pipe(wakeup_) == 0) {
            for (const auto fd : wakeup_) {
                if (::fcntl(fd, F_SETFL, O_NONBLOCK) == -1 ||
                    ::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
                    ::close(wakeup_[0]);
                    ::close(wakeup_[1]);
                    wakeup_[0] = wakeup_[1] = -1;
                    break;
                }
            }
        }
        // Convenience overload post(task) is local to the loop's owning thread.
        next_loop_ = current_;
        if (current_) current_->previous_loop_ = this;
        current_ = this;
    }

    Loop(const Loop&) = delete;
    auto operator=(const Loop&) -> Loop& = delete;

    ~Loop() {
        if (wakeup_[0] >= 0) ::close(wakeup_[0]);
        if (wakeup_[1] >= 0) ::close(wakeup_[1]);
        if (previous_loop_) previous_loop_->next_loop_ = next_loop_;
        else current_ = next_loop_;
        if (next_loop_) next_loop_->previous_loop_ = previous_loop_;
    }

    [[nodiscard]] auto watch(const int fd, const short events, Callback callback) -> FdWatch {
        if (fd < 0 || !callback) return {};
        auto watch = std::make_shared<Watch>(Watch{fd, std::make_shared<short>(events), std::move(callback)});
        watches_.emplace(next_id_++, watch);
        return FdWatch{watch->active, watch->events};
    }

    [[nodiscard]] auto after(const std::chrono::milliseconds delay, Task callback) -> Timer {
        return schedule(delay, std::chrono::milliseconds::zero(), std::move(callback));
    }

    [[nodiscard]] auto every(const std::chrono::milliseconds interval, Task callback) -> Timer {
        return interval.count() > 0 ? schedule(interval, interval, std::move(callback)) : Timer{};
    }

    [[nodiscard]] auto idle(Task callback) -> Idle {
        if (!callback) return {};
        auto idle = std::make_shared<IdleTask>(IdleTask{std::move(callback)});
        idles_.push_back(idle);
        return Idle{idle->active};
    }

    void run_once(const std::chrono::milliseconds timeout = std::chrono::milliseconds{50}) {
        // Reentrant callbacks may post further work, but cannot start a second
        // dispatch batch on the same loop while the first is still active.
        if (dispatching_) return;
        struct DispatchGuard {
            bool& flag;
            ~DispatchGuard() { flag = false; }
        } guard{dispatching_};
        dispatching_ = true;
        drain_wakeup();
        const auto worked = run_tasks();
        const auto timer_fired = fire_timers();
        auto wait = std::max(timeout, std::chrono::milliseconds::zero());
        if (worked || timer_fired) wait = std::chrono::milliseconds::zero();
        for (const auto& timer : timers_) {
            if (!timer->active->load()) continue;
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(timer->due - Clock::now());
            wait = std::min(wait, std::max(remaining, std::chrono::milliseconds::zero()));
        }
        std::vector<::pollfd> descriptors;
        std::vector<std::shared_ptr<Watch>> polled;
        if (wakeup_[0] >= 0) {
            descriptors.push_back({wakeup_[0], POLLIN, 0});
            polled.push_back(nullptr);
        }
        for (const auto& [id, watch] : watches_) {
            if (!watch->active->load()) continue;
            descriptors.push_back({watch->fd, *watch->events, 0});
            polled.push_back(watch);
        }
        const auto result = ::poll(descriptors.data(), descriptors.size(),
                                   static_cast<int>(std::min<std::chrono::milliseconds::rep>(
                                       wait.count(), std::numeric_limits<int>::max())));
        if (result > 0) {
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                const auto flags = descriptors[index].revents;
                const auto& watch = polled[index];
                if (!flags) continue;
                if (!watch) { drain_wakeup(); continue; }
                if (!watch->active->load()) continue;
                if (flags & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
                    watch->callback({Event::Kind::readable, watch->fd, 0, flags});
                if (watch->active->load() && (flags & POLLOUT))
                    watch->callback({Event::Kind::writable, watch->fd, 0, flags});
                if (flags & POLLNVAL) watch->active->store(false);
            }
        }
        const auto late_timer = !timer_fired && fire_timers();
        if (!worked && !timer_fired && !late_timer && result == 0) {
            const auto idle_snapshot = idles_;
            for (const auto& idle : idle_snapshot)
                if (idle->active->load()) idle->callback();
        }
        (void)run_tasks();
        std::erase_if(watches_, [](const auto& item) { return !item.second->active->load(); });
        std::erase_if(idles_, [](const auto& item) { return !item->active->load(); });
    }

    void run() {
        if (dispatching_ || running_) return;
        running_ = true;
        try {
            while (running_) run_once();
        } catch (...) {
            running_ = false;
            throw;
        }
    }

    void stop() noexcept { running_ = false; wake(); }
    [[nodiscard]] auto running() const noexcept -> bool { return running_.load(); }
    [[nodiscard]] static auto current() noexcept -> Loop* { return current_; }

    void enqueue(Task task) {
        if (!task) return;
        {
            const std::lock_guard lock(tasks_mutex_);
            tasks_.push_back(std::move(task));
        }
        wake();
    }

private:
    using Clock = std::chrono::steady_clock;
    using State = std::shared_ptr<std::atomic<bool>>;

    struct Watch {
        int fd;
        std::shared_ptr<short> events;
        Callback callback;
        State active = std::make_shared<std::atomic<bool>>(true);
    };
    struct Scheduled {
        Clock::time_point due;
        std::chrono::milliseconds repeat;
        Task callback;
        State active = std::make_shared<std::atomic<bool>>(true);
    };
    struct IdleTask {
        Task callback;
        State active = std::make_shared<std::atomic<bool>>(true);
    };

    [[nodiscard]] auto schedule(const std::chrono::milliseconds delay,
                                const std::chrono::milliseconds repeat, Task callback) -> Timer {
        if (!callback) return {};
        auto timer = std::make_shared<Scheduled>(Scheduled{Clock::now() + delay, repeat, std::move(callback)});
        timers_.push_back(timer);
        return Timer{timer->active};
    }

    auto fire_timers() -> bool {
        const auto now = Clock::now();
        const auto snapshot = timers_;
        bool fired = false;
        for (const auto& timer : snapshot) {
            if (!timer->active->load() || timer->due > now) continue;
            if (timer->repeat.count() == 0) timer->active->store(false);
            else timer->due = now + timer->repeat;
            timer->callback();
            fired = true;
        }
        std::erase_if(timers_, [](const auto& item) { return !item->active->load(); });
        return fired;
    }

    auto run_tasks() -> bool {
        // Bound each dispatch batch without losing pending tasks if a callback throws.
        std::unique_lock lock(tasks_mutex_);
        auto remaining = tasks_.size();
        if (remaining == 0) return false;
        while (remaining-- != 0) {
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            task();
            lock.lock();
        }
        return true;
    }

    void wake() noexcept {
        if (wakeup_[1] >= 0) {
            const std::uint8_t byte = 1;
            while (::write(wakeup_[1], &byte, sizeof(byte)) < 0 && errno == EINTR) {}
        }
    }

    void drain_wakeup() {
        std::uint8_t bytes[64]{};
        if (wakeup_[0] >= 0)
            while (::read(wakeup_[0], bytes, sizeof(bytes)) > 0) {}
    }

    inline static thread_local Loop* current_{};
    Loop* previous_loop_{};
    Loop* next_loop_{};
    int wakeup_[2]{-1, -1};
    std::size_t next_id_{1};
    std::map<std::size_t, std::shared_ptr<Watch>> watches_;
    std::vector<std::shared_ptr<Scheduled>> timers_;
    std::vector<std::shared_ptr<IdleTask>> idles_;
    std::mutex tasks_mutex_;
    std::deque<Task> tasks_;
    std::atomic<bool> running_{};
    bool dispatching_{};
};

inline void post(Loop& loop, Loop::Task task) { loop.enqueue(std::move(task)); }

/// Posts to the most recently constructed live loop on the calling thread.
inline void post(Loop::Task task) {
    if (Loop::current() != nullptr) Loop::current()->enqueue(std::move(task));
}

}  // namespace ttyutils::loop
