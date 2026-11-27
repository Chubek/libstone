#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>

#include <chrono>
#include <functional>
#include <limits>
#include <optional>

namespace ttytk {

/** @brief Dispatch mouse reports, optionally in a region's coordinate space. */
class MouseListener {
public:
    using Clock = std::chrono::steady_clock;

    struct Region {
        unsigned x{1};
        unsigned y{1};
        unsigned width{};
        unsigned height{};
    };

    std::function<void(const input::Mouse&)> on_move{};
    std::function<void(const input::Mouse&)> on_click{};
    std::function<void(const input::Mouse&)> on_drag{};
    std::function<void(const input::Mouse&)> on_release{};
    /** @brief Optional custom filter, evaluated in terminal coordinates. */
    std::function<bool(const input::Mouse&)> hit_test{};

    /** @brief When set, filter to this region and deliver one-based local coordinates. */
    inline void set_region(const std::optional<Region> region) noexcept {
        region_ = region;
        last_click_.reset();
        clicks_ = 0;
    }

    /** @brief Count consecutive presses of the same button at the same location. */
    [[nodiscard]] inline auto click_count() const noexcept -> unsigned { return clicks_; }

    std::chrono::milliseconds click_interval{500};

    inline void dispatch(input::Mouse mouse, const Clock::time_point now = Clock::now()) {
        if (hit_test && !hit_test(mouse)) return;
        if (region_) {
            if (mouse.x < region_->x || mouse.y < region_->y ||
                mouse.x - region_->x >= region_->width || mouse.y - region_->y >= region_->height) {
                return;
            }
            mouse.x = mouse.x - region_->x + 1;
            mouse.y = mouse.y - region_->y + 1;
        }
        if (mouse.motion) {
            if (mouse.pressed) {
                last_click_.reset();
                clicks_ = 0;
                if (on_drag) on_drag(mouse);
            } else if (on_move) {
                on_move(mouse);
            }
        } else if (mouse.pressed) {
            if (last_click_ && last_click_->mouse.button == mouse.button &&
                last_click_->mouse.x == mouse.x && last_click_->mouse.y == mouse.y &&
                now >= last_click_->time && now - last_click_->time <= click_interval) {
                if (clicks_ < std::numeric_limits<unsigned>::max()) ++clicks_;
            } else {
                clicks_ = 1;
            }
            last_click_ = Click{mouse, now};
            if (on_click) on_click(mouse);
        } else if (on_release) {
            on_release(mouse);
        }
    }

private:
    struct Click {
        input::Mouse mouse;
        Clock::time_point time;
    };
    std::optional<Region> region_{};
    std::optional<Click> last_click_{};
    unsigned clicks_{};
};

}  // namespace ttytk
