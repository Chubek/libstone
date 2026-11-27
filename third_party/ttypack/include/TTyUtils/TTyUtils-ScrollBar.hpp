#pragma once

#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ui {

enum Orientation { Vertical, Horizontal };

struct ScrollMark {
    std::size_t offset{};
    theme::Style style{};
};

class Scrollable {
public:
    virtual ~Scrollable() = default;
    [[nodiscard]] virtual auto offset() const -> std::size_t = 0;
    virtual void scroll_to(std::size_t offset) = 0;
    [[nodiscard]] virtual auto length() const -> std::size_t = 0;
    /** Number of items visible at once. A legacy scrollable defaults to a one-item page. */
    [[nodiscard]] virtual auto visible_length() const -> std::size_t { return 1; }
    [[nodiscard]] auto max_offset() const -> std::size_t {
        const auto total = length();
        return total - std::min(total, std::max(std::size_t{1}, visible_length()));
    }
};

class ScrollBar : public Widget {
public:
    explicit ScrollBar(const Orientation orientation = Vertical) : orientation_(orientation) {}
    /** The attached viewer must outlive the attachment. */
    void attach(Scrollable& scrollable) { scrollable_ = &scrollable; }
    void detach() noexcept { scrollable_ = nullptr; }
    void set_marks(std::vector<ScrollMark> marks) { marks_ = std::move(marks); }
    void set_ascii(const bool value) noexcept { ascii_ = value; }
    [[nodiscard]] auto fraction() const -> double {
        if (!scrollable_) return 0.0;
        const auto maximum = scrollable_->max_offset();
        return maximum == 0 ? 0.0 : static_cast<double>(std::min(scrollable_->offset(), maximum)) /
                                        static_cast<double>(maximum);
    }
    void scroll_by(const int amount) {
        if (!scrollable_) return;
        const auto maximum = scrollable_->max_offset();
        const auto current = std::min(scrollable_->offset(), maximum);
        if (amount < 0) {
            const auto step = static_cast<std::size_t>(-(amount + 1)) + 1;
            scrollable_->scroll_to(current - std::min(current, step));
        } else {
            const auto step = static_cast<std::size_t>(amount);
            scrollable_->scroll_to(current + std::min(maximum - current, step));
        }
    }

    /** Map a click or drag along the track to a scroll position. */
    void scroll_at(const int cell) {
        if (!scrollable_ || extent() <= 0) return;
        const auto maximum = scrollable_->max_offset();
        if (cell <= 0 || extent() == 1) scrollable_->scroll_to(0);
        else if (cell >= extent() - 1) scrollable_->scroll_to(maximum);
        else {
            const auto ratio = static_cast<long double>(cell) / (extent() - 1);
            scrollable_->scroll_to(static_cast<std::size_t>(ratio * maximum));
        }
    }

    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* mouse = std::get_if<ttytk::input::Mouse>(&event);
        if (!mouse || !scrollable_) return false;
        int x{}, y{};
        if (!mouse_cell(*mouse, x, y)) return false;
        const auto inside = rect().contains(x, y);
        if (inside && mouse->button >= 64 && mouse->button <= 67 && mouse->pressed) {
            if (mouse->button >= 66 && orientation_ != Horizontal) return false;
            scroll_by((mouse->button & 1U) != 0 ? 3 : -3);
            return true;
        }
        if (mouse->button != 0 && !(dragging_ && mouse->button == 3 && !mouse->pressed)) return false;
        if (!mouse->pressed) return std::exchange(dragging_, false);
        if (!mouse->motion) dragging_ = inside;
        if (!dragging_) return false;
        const auto point = orientation_ == Horizontal ? x : y;
        const auto origin = orientation_ == Horizontal ? rect().x : rect().y;
        const auto cell = point <= origin ? 0LL : static_cast<long long>(point) - origin;
        scroll_at(static_cast<int>(std::min<long long>(extent(), cell)));
        return true;
    }

    void paint(Canvas& canvas) override {
        if (!scrollable_ || rect().width <= 0 || rect().height <= 0) return;
        const auto total = scrollable_->length();
        const auto cells = extent();
        if (!total) return;
        const auto page = std::clamp(scrollable_->visible_length(), std::size_t{1}, total);
        const auto thumb = std::clamp(static_cast<double>(cells) * (static_cast<double>(page) / total),
                                      1.0, static_cast<double>(cells));
        const auto start = (cells - thumb) * fraction();
        const auto end = start + thumb;
        constexpr std::string_view horizontal[]{"", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
        constexpr std::string_view vertical[]{"", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        const auto ascii = ascii_ || !canvas.unicode();
        for (int cell = 0; cell < cells; ++cell) {
            const auto coverage = std::max(0.0, std::min(end, cell + 1.0) - std::max(start, static_cast<double>(cell)));
            const auto blocks = std::clamp(static_cast<int>(coverage * 8 + 0.5), 0, 8);
            auto glyph = orientation_ == Vertical ? std::string_view{"│"} : std::string_view{"─"};
            auto cell_style = style();
            if (blocks > 0) glyph = orientation_ == Vertical ? vertical[blocks] : horizontal[blocks];
            if (ascii) glyph = blocks > 0 ? "#" : orientation_ == Vertical ? "|" : "-";
            for (const auto& mark : marks_) {
                if (mark.offset >= total) continue;
                const auto track = static_cast<long double>(mark.offset) / total;
                if (static_cast<int>(track * cells) != cell) continue;
                cell_style = mark.style;
                if (blocks == 0) glyph = ascii ? "*" : "•";
            }
            canvas.text(rect().x + (orientation_ == Horizontal ? cell : 0),
                        rect().y + (orientation_ == Vertical ? cell : 0), glyph, cell_style);
        }
    }

private:
    [[nodiscard]] auto extent() const noexcept -> int {
        return orientation_ == Vertical ? rect().height : rect().width;
    }
    Orientation orientation_;
    Scrollable* scrollable_{};
    std::vector<ScrollMark> marks_;
    bool ascii_{};
    bool dragging_{};
};

}  // namespace ttyutils::ui
