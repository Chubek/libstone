#pragma once

#include "state.hpp"

#include <cstddef>
#include <deque>
#include <mutex>

namespace domios {

enum class EventKind : std::uint32_t {
    log = 1,
    diagnostic = 2,
    domainlet_error = 3,
    plugin_notice = 4,
    shell_output = 5
};

struct Event {
    EventKind kind{EventKind::log};
    std::uint32_t code{};
    std::string subject;
    std::string message;
};

class EventQueue {
public:
    explicit EventQueue(std::size_t capacity);
    bool try_push(Event event) noexcept;
    bool try_pop(Event& event) noexcept;
    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    std::deque<Event> queue_;
    mutable std::mutex mutex_;
};

} // namespace domios

