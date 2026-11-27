#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-KeyListener.hpp>
#include <TTyTk/TTyTk-MouseListener.hpp>

#include <deque>
#include <functional>
#include <queue>
#include <string>
#include <utility>
#include <variant>

namespace ttytk {

/** @brief A terminal size change in character cells. */
struct Resize {
    unsigned rows{};
    unsigned columns{};
};

/** @brief An application-defined event identified by name. */
struct UserEvent {
    std::string name{};
};

using Event = std::variant<input::Key, input::Mouse, input::Paste, Resize, UserEvent>;

/** @brief Queue events for polling and deliver them synchronously to subscribers. */
class EventBus {
public:
    /** @brief Queue first so callbacks can poll the posted event. */
    inline void post(Event event) {
        events_.push(event);
        const auto count = subscribers_.size();
        for (std::size_t index = 0; index < count; ++index) subscribers_[index](event);
    }

    /** @brief Subscribe beginning with the next post; an empty callback is ignored. */
    inline void subscribe(std::function<void(const Event&)> callback) {
        if (callback) subscribers_.push_back(std::move(callback));
    }

    /** @brief Pop the oldest event without notifying subscribers again. */
    inline auto poll(Event& event) -> bool {
        if (events_.empty()) return false;
        event = std::move(events_.front());
        events_.pop();
        return true;
    }

private:
    std::queue<Event> events_{};
    std::deque<std::function<void(const Event&)>> subscribers_{};
};

}  // namespace ttytk
