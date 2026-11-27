#pragma once

#include <TTyUtils/TTyUtils-Theme.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ttyutils::ui {

enum class Align { Left, Center, Right };

struct Segment {
    std::string text{};
    theme::Style style{};
    int priority{};
    std::function<void()> action{};
    std::optional<std::chrono::steady_clock::time_point> expires{};
};

class StatusBar : public Widget {
public:
    void left(std::vector<Segment> segments) { left_ = std::move(segments); }
    void center(std::vector<Segment> segments) { center_ = std::move(segments); }
    void right(std::vector<Segment> segments) { right_ = std::move(segments); }
    void set_separators(const bool enabled, const bool ascii = false) noexcept {
        separator_ = enabled ? (ascii ? "|" : "") : "";
    }
    void flash(std::string message, const std::chrono::milliseconds duration = std::chrono::seconds{2}) {
        transient_ = std::move(message);
        transient_until_ = std::chrono::steady_clock::now() + duration;
    }
    [[nodiscard]] auto message() const -> std::string {
        return std::chrono::steady_clock::now() < transient_until_ ? transient_ : std::string{};
    }
    void paint(Canvas& canvas) override {
        if (rect().width <= 0 || rect().height <= 0) return;
        const auto current_message = message();
        if (!current_message.empty()) {
            canvas.text(rect().x, rect().y, fit_text(current_message, rect().width), style());
            return;
        }
        for (const auto& item : placements()) {
            if (item.separator)
                canvas.text(rect().x + item.x - cell_width(separator_), rect().y, separator_, item.segment->style);
            canvas.text(rect().x + item.x, rect().y, fit_text(item.segment->text, item.width), item.segment->style);
        }
    }
    /** Invoke the segment on the supplied zero-based UI cell, if actionable. */
    auto click(const int x, const int y) -> bool {
        if (y != rect().y || !rect().contains(x, y) || !message().empty()) return false;
        for (const auto& item : placements()) {
            if (x - rect().x >= item.x && x - rect().x - item.x < item.width) {
                const auto action = item.segment->action;
                if (action) { action(); return true; }
                return false;
            }
        }
        return false;
    }
    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* mouse = std::get_if<ttytk::input::Mouse>(&event);
        int x{}, y{};
        return mouse && mouse->button == 0 && mouse->pressed && !mouse->motion &&
               mouse_cell(*mouse, x, y) && click(x, y);
    }

private:
    struct Placement { const Segment* segment; int x; int width; bool separator; };
    [[nodiscard]] auto placements() const -> std::vector<Placement> {
        const std::array groups{&left_, &center_, &right_};
        std::array<std::vector<Placement>, 3> items;
        std::array<std::int64_t, 3> widths{};
        const auto now = std::chrono::steady_clock::now();
        const auto space = cell_width(separator_);
        std::size_t count = 0;
        for (std::size_t group = 0; group < groups.size(); ++group) {
            for (const auto& segment : *groups[group]) {
                if (segment.expires && now >= *segment.expires) continue;
                const auto width = cell_width(segment.text);
                if (width == 0) continue;
                if (!items[group].empty()) widths[group] += space;
                widths[group] += width;
                items[group].push_back({&segment, 0, width, false});
                ++count;
            }
        }
        const auto total = [&] { return widths[0] + widths[1] + widths[2]; };
        while (total() > rect().width && count > 1) {
            std::size_t drop_group = 0;
            std::size_t drop_item = 0;
            const Segment* lowest = nullptr;
            for (std::size_t group = 0; group < items.size(); ++group)
                for (std::size_t i = 0; i < items[group].size(); ++i)
                    if (!lowest || items[group][i].segment->priority <= lowest->priority) {
                        lowest = items[group][i].segment;
                        drop_group = group;
                        drop_item = i;
                    }
            auto& group = items[drop_group];
            widths[drop_group] -= group[drop_item].width + (group.size() > 1 ? space : 0);
            group.erase(group.begin() + static_cast<std::ptrdiff_t>(drop_item));
            --count;
        }
        if (count == 0) return {};
        if (total() > rect().width) {
            for (std::size_t group = 0; group < items.size(); ++group)
                if (!items[group].empty()) items[group].front().width = widths[group] = rect().width;
        }
        const auto right = rect().width - static_cast<int>(widths[2]);
        const auto center_width = static_cast<int>(widths[1]);
        const auto center = std::clamp((rect().width - center_width) / 2,
                                       static_cast<int>(widths[0]), right - center_width);
        std::array positions{0, center, right};
        std::vector<Placement> result;
        for (std::size_t group = 0; group < items.size(); ++group) {
            bool first = true;
            for (auto item : items[group]) {
                item.separator = !first && space != 0;
                if (item.separator) positions[group] += space;
                item.x = positions[group];
                positions[group] += item.width;
                result.push_back(item);
                first = false;
            }
        }
        return result;
    }
    std::vector<Segment> left_;
    std::vector<Segment> center_;
    std::vector<Segment> right_;
    std::string transient_;
    std::string_view separator_;
    std::chrono::steady_clock::time_point transient_until_{};
};

inline auto transient(std::string text, const std::chrono::milliseconds duration = std::chrono::seconds{2}) -> Segment {
    return Segment{.text = std::move(text), .expires = std::chrono::steady_clock::now() + duration};
}

}  // namespace ttyutils::ui
