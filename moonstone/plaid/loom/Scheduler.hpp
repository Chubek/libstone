#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace moonstone::plaid::loom {
class CancellationToken {
    std::shared_ptr<std::atomic<bool>> cancelled_;

  public:
    CancellationToken() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}
    void request() const noexcept { cancelled_->store(true, std::memory_order_release); }
    bool requested() const noexcept { return cancelled_->load(std::memory_order_acquire); }
};

// Deterministic cooperative scheduler. All callbacks run on the calling thread.
class CooperativeScheduler {
    std::deque<std::function<void()>> ready_;
    bool running_ = false;

  public:
    void post(std::function<void()> fn) {
        if (!fn)
            throw std::invalid_argument("empty task");
        ready_.push_back(std::move(fn));
    }
    std::size_t pending() const noexcept { return ready_.size(); }
    std::size_t run(std::size_t budget = 100000) {
        if (running_)
            throw std::logic_error("nested scheduler run");
        running_ = true;
        std::size_t executed = 0;
        try {
            while (!ready_.empty() && executed < budget) {
                auto fn = std::move(ready_.front());
                ready_.pop_front();
                ++executed;
                fn();
            }
        } catch (...) {
            running_ = false;
            throw;
        }
        running_ = false;
        return executed;
    }
};

// M:N task executor with per-worker deques. Work-stealing is lock protected.
// Shutdown drains already accepted tasks; tasks should be finite and nonblocking.
class WorkStealingPool {
    struct Worker {
        std::mutex mutex;
        std::deque<std::function<void()>> queue;
    };
    std::vector<std::unique_ptr<Worker>> queues_;
    std::vector<std::thread> workers_;
    std::mutex admission_;
    std::mutex join_mutex_;
    std::condition_variable ready_cv_;
    std::atomic<std::size_t> queued_{0};
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> stopping_{false};
    const std::size_t queue_limit_;
    inline static thread_local WorkStealingPool *current_pool_ = nullptr;
    bool pop(std::size_t id, std::function<void()> &result) {
        for (std::size_t offset = 0; offset < queues_.size(); ++offset) {
            const auto index = (id + offset) % queues_.size();
            std::lock_guard lock(queues_[index]->mutex);
            auto &q = queues_[index]->queue;
            if (q.empty())
                continue;
            if (offset == 0) {
                result = std::move(q.back());
                q.pop_back();
            } else {
                result = std::move(q.front());
                q.pop_front();
            }
            queued_.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
        return false;
    }
    void worker(std::size_t id) {
        current_pool_ = this;
        for (;;) {
            std::function<void()> job;
            if (pop(id, job)) {
                job();
                continue;
            }
            // Admission and stop publish under the same mutex as this wait.
            // Atomics alone do not prevent a notification racing entry to wait.
            std::unique_lock lock(admission_);
            ready_cv_.wait(lock, [&] {
                return stopping_.load(std::memory_order_acquire) ||
                       queued_.load(std::memory_order_acquire) > 0;
            });
            if (stopping_.load(std::memory_order_acquire) &&
                queued_.load(std::memory_order_acquire) == 0) {
                current_pool_ = nullptr;
                return;
            }
        }
    }

  public:
    explicit WorkStealingPool(std::size_t workers = std::thread::hardware_concurrency(),
                              std::size_t queue_limit = 0)
        : queue_limit_(queue_limit) {
        if (!workers)
            workers = 1;
        for (std::size_t i = 0; i < workers; ++i)
            queues_.push_back(std::make_unique<Worker>());
        try {
            for (std::size_t i = 0; i < workers; ++i)
                workers_.emplace_back([this, i] { worker(i); });
        } catch (...) {
            stop();
            throw;
        }
    }
    WorkStealingPool(const WorkStealingPool &) = delete;
    WorkStealingPool &operator=(const WorkStealingPool &) = delete;
    ~WorkStealingPool() { stop(); }
    std::size_t workers() const noexcept { return queues_.size(); }
    template <class Fn>
    auto submit(Fn &&fn) -> std::future<std::invoke_result_t<std::decay_t<Fn> &>> {
        using Result = std::invoke_result_t<std::decay_t<Fn> &>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        auto future = task->get_future();
        {
            std::lock_guard admission(admission_);
            if (stopping_.load(std::memory_order_acquire))
                throw std::runtime_error("pool stopped");
            if (queue_limit_ && queued_.load(std::memory_order_acquire) >= queue_limit_)
                throw std::runtime_error("pool queue limit reached");
            const auto id = next_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
            std::lock_guard queue_lock(queues_[id]->mutex);
            queues_[id]->queue.emplace_back([task] { (*task)(); });
            queued_.fetch_add(1, std::memory_order_release);
        }
        ready_cv_.notify_one();
        return future;
    }
    // Nonjoining stop request: safe in a worker and does not interrupt callbacks.
    void request_stop() noexcept {
        {
            std::lock_guard admission(admission_);
            stopping_.store(true, std::memory_order_release);
        }
        ready_cv_.notify_all();
    }
    void stop() {
        // Check before taking join_mutex_: an external joiner may be waiting
        // for this worker. Joining or waiting on that mutex here would deadlock.
        if (current_pool_ == this)
            throw std::logic_error("pool worker cannot join itself; use request_stop");
        request_stop();
        std::lock_guard joining(join_mutex_);
        for (auto &w : workers_)
            if (w.joinable())
                w.join();
        workers_.clear();
    }
};

class TaskScope {
    WorkStealingPool &pool_;
    CancellationToken cancellation_;
    std::vector<std::future<void>> children_;

  public:
    explicit TaskScope(WorkStealingPool &p) : pool_(p) {}
    TaskScope(const TaskScope &) = delete;
    TaskScope &operator=(const TaskScope &) = delete;
    template <class Fn> void spawn(Fn fn) {
        // Allocation must precede admission or an allocation failure could
        // leave an accepted task untracked by this structured scope.
        children_.emplace_back();
        auto token = cancellation_;
        try {
            children_.back() = pool_.submit([fn = std::move(fn), token]() mutable {
                if (!token.requested()) {
                    try {
                        fn(token);
                    } catch (...) {
                        token.request();
                        throw;
                    }
                }
            });
        } catch (...) {
            children_.pop_back();
            throw;
        }
    }
    void cancel() noexcept { cancellation_.request(); }
    void join() {
        std::exception_ptr first;
        for (auto &f : children_) {
            try {
                f.get();
            } catch (...) {
                if (!first)
                    first = std::current_exception();
                cancellation_.request();
            }
        }
        children_.clear();
        if (first)
            std::rethrow_exception(first);
    }
    ~TaskScope() noexcept {
        cancel();
        for (auto &f : children_)
            if (f.valid())
                f.wait();
    }
};
} // namespace moonstone::plaid::loom
