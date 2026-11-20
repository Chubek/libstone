#include "event_queue.hpp"

#include <utility>

namespace domios {

EventQueue::EventQueue(std::size_t capacity) : capacity_(capacity ? capacity : 1u) {}

bool EventQueue::try_push(Event event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_) {
        if (!queue_.empty() && queue_.back().kind == event.kind) {
            queue_.back() = std::move(event);
            return false;
        }
        queue_.pop_front();
    }
    queue_.push_back(std::move(event));
    return true;
}

bool EventQueue::try_pop(Event& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    event = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

std::size_t EventQueue::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

} // namespace domios

