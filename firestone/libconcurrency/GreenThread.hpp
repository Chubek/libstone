#pragma once
#include "Fiber.hpp"
#include <cstddef>
#include <limits>
#include <list>
#include <map>
#include <memory>

namespace firestone::concurrency {

// Single-threaded round-robin executor. Tasks may spawn or cancel other tasks.
class GreenThread {
public:
    using TaskId = std::size_t;
    GreenThread() = default;
    GreenThread(const GreenThread&) = delete;
    GreenThread& operator=(const GreenThread&) = delete;
    TaskId spawn(std::function<FiberStep()> step) {
        if (next_ == std::numeric_limits<TaskId>::max()) throw std::overflow_error("task IDs exhausted");
        auto fiber = std::make_shared<Fiber>(std::move(step)); auto id = next_;
        tasks_.emplace(id, std::move(fiber));
        try { ready_.push_back(id); } catch (...) { tasks_.erase(id); throw; }
        ++next_; return id;
    }
    bool cancel(TaskId id) {
        auto found = tasks_.find(id); if (found == tasks_.end()) return false;
        found->second->cancel(); return true;
    }
    std::size_t pending() const noexcept { return tasks_.size(); }
    std::size_t run(std::size_t budget = std::numeric_limits<std::size_t>::max()) {
        if (running_) throw std::logic_error("recursive scheduler run");
        running_ = true; std::size_t steps = 0;
        try {
            while (!ready_.empty() && steps < budget) {
                auto id = ready_.front();
                auto found = tasks_.find(id);
                if (found == tasks_.end()) { ready_.pop_front(); continue; }
                auto fiber = found->second;
                try { fiber->resume(); } catch (...) { ready_.pop_front(); tasks_.erase(id); throw; }
                ++steps;
                if (fiber->done()) { ready_.pop_front(); tasks_.erase(id); }
                else ready_.splice(ready_.end(), ready_, ready_.begin());
            }
        } catch (...) { running_ = false; throw; }
        running_ = false; return steps;
    }
private:
    std::map<TaskId, std::shared_ptr<Fiber>> tasks_;
    std::list<TaskId> ready_;
    TaskId next_ = 0;
    bool running_ = false;
};
} // namespace firestone::concurrency
