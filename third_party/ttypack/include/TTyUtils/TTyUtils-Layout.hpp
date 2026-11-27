#pragma once

#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace ttyutils::ui {

struct Constraint {
    int minimum{};
    int maximum{0x3fffffff};
    int stretch{1};
};

namespace detail {

[[nodiscard]] constexpr auto add_extents(const int a, const int b) noexcept -> int {
    return a + std::min(std::max(0, b), std::numeric_limits<int>::max() - a);
}

[[nodiscard]] inline auto measure_box(const Widget& box, const bool horizontal) -> SizeHint {
    SizeHint result;
    const auto accumulate = [horizontal](SizeHint::Size& total, const SizeHint::Size part) {
        if (horizontal) {
            total.width = add_extents(total.width, part.width);
            total.height = std::max(total.height, part.height);
        } else {
            total.width = std::max(total.width, part.width);
            total.height = add_extents(total.height, part.height);
        }
    };
    for (const auto& child : box.children()) {
        if (!child->visible()) continue;
        const auto hint = child->hint();
        accumulate(result.min, hint.min);
        accumulate(result.preferred, {std::max(hint.min.width, hint.preferred.width),
                                      std::max(hint.min.height, hint.preferred.height)});
    }
    return result;
}

/// Proportionally assigns exactly `total` cells, with stable left-to-right ties.
[[nodiscard]] inline auto distribute(const int total, const std::vector<int>& weights) -> std::vector<int> {
    std::vector<int> result(weights.size());
    const auto sum = std::accumulate(weights.begin(), weights.end(), std::int64_t{0});
    if (sum <= 0 || total <= 0) return result;
    std::vector<std::pair<std::int64_t, std::size_t>> fractions;
    fractions.reserve(weights.size());
    int remainder = total;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        const auto product = static_cast<std::int64_t>(weights[i]) * total;
        result[i] = static_cast<int>(product / sum);
        remainder -= result[i];
        fractions.emplace_back(product % sum, i);
    }
    std::stable_sort(fractions.begin(), fractions.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    for (int i = 0; i < remainder; ++i) ++result[fractions[static_cast<std::size_t>(i)].second];
    return result;
}

inline void layout_box(Widget& box, const bool horizontal) {
    const auto bounds = box.rect();
    const auto extent = std::max(0, horizontal ? bounds.width : bounds.height);
    std::vector<int> minima, preferences, stretch;
    for (const auto& child : box.children()) {
        if (!child->visible()) continue;
        const auto hint = child->hint();
        minima.push_back(std::max(0, horizontal ? hint.min.width : hint.min.height));
        preferences.push_back(std::max(minima.back(), horizontal ? hint.preferred.width : hint.preferred.height));
        stretch.push_back(std::max(0, hint.stretch));
    }
    const auto min_sum = std::accumulate(minima.begin(), minima.end(), std::int64_t{0});
    const auto preferred_sum = std::accumulate(preferences.begin(), preferences.end(), std::int64_t{0});
    std::vector<int> sizes;
    if (extent < min_sum) {
        sizes = distribute(extent, minima);
    } else if (extent < preferred_sum) {
        auto flexibility = preferences;
        for (std::size_t i = 0; i < flexibility.size(); ++i) flexibility[i] -= minima[i];
        sizes = distribute(extent - static_cast<int>(min_sum), flexibility);
        for (std::size_t i = 0; i < sizes.size(); ++i) sizes[i] += minima[i];
    } else {
        sizes = distribute(extent - static_cast<int>(preferred_sum), stretch);
        for (std::size_t i = 0; i < sizes.size(); ++i) sizes[i] += preferences[i];
    }
    std::size_t i = 0;
    auto at = horizontal ? bounds.x : bounds.y;
    for (const auto& child : box.children()) {
        if (!child->visible()) continue;
        const auto size = sizes[i++];
        child->set_rect(horizontal ? Rect{at, bounds.y, size, bounds.height}
                                   : Rect{bounds.x, at, bounds.width, size});
        at += size;
    }
}

[[nodiscard]] inline auto track_edge(const std::size_t index, const std::size_t tracks,
                                     const int extent) noexcept -> int {
    const auto cells = static_cast<std::size_t>(std::max(0, extent));
    return static_cast<int>(index * (cells / tracks) + std::min(index, cells % tracks));
}

}  // namespace detail

class HBox : public Widget {
public:
    HBox() { set_focusable(false); }
    explicit HBox(std::vector<std::unique_ptr<Widget>> children) : HBox() {
        for (auto& child : children) add_child(std::move(child));
    }
    void layout() override { detail::layout_box(*this, true); }
    [[nodiscard]] auto hint() const -> SizeHint override { return detail::measure_box(*this, true); }
};

class VBox : public Widget {
public:
    VBox() { set_focusable(false); }
    explicit VBox(std::vector<std::unique_ptr<Widget>> children) : VBox() {
        for (auto& child : children) add_child(std::move(child));
    }
    void layout() override { detail::layout_box(*this, false); }
    [[nodiscard]] auto hint() const -> SizeHint override { return detail::measure_box(*this, false); }
};

class Grid : public Widget {
public:
    struct Placement {
        std::size_t row{}, column{}, rows{1}, columns{1};
    };

    Grid(const std::size_t rows = 1, const std::size_t columns = 1)
        : rows_(std::max<std::size_t>(rows, 1)), columns_(std::max<std::size_t>(columns, 1)) {
        set_focusable(false);
    }

    /// Overrides the default row-major placement for a child index.
    void place(const std::size_t index, const Placement placement) {
        if (index >= children().size()) return;
        if (placements_.size() <= index) placements_.resize(index + 1);
        placements_[index] = placement;
        invalidate(rect());
    }

    void layout() override {
        const auto bounds = rect();
        for (std::size_t i = 0; i < children().size(); ++i) {
            auto placement = Placement{i / columns_, i % columns_, 1, 1};
            if (i < placements_.size() && placements_[i]) placement = *placements_[i];
            const auto [row, column, rows, columns] = placement;
            if (row >= rows_ || column >= columns_ || rows == 0 || columns == 0) {
                children()[i]->set_rect({});
                continue;
            }
            const auto x = detail::track_edge(column, columns_, bounds.width);
            const auto y = detail::track_edge(row, rows_, bounds.height);
            const auto right = detail::track_edge(column + std::min(columns, columns_ - column), columns_, bounds.width);
            const auto bottom = detail::track_edge(row + std::min(rows, rows_ - row), rows_, bounds.height);
            children()[i]->set_rect({bounds.x + x, bounds.y + y, right - x, bottom - y});
        }
    }

private:
    std::size_t rows_;
    std::size_t columns_;
    std::vector<std::optional<Placement>> placements_;
};

class Stack : public Widget {
public:
    Stack() { set_focusable(false); }
    void layout() override {
        for (const auto& child : children()) child->set_rect(rect());
    }
};

class Spacer : public Widget {
public:
    explicit Spacer(const int stretch = 1) : stretch_(stretch) { set_focusable(false); }
    [[nodiscard]] auto hint() const -> SizeHint override { return {{0, 0}, {0, 0}, stretch_}; }

private:
    int stretch_;
};

class Split : public Widget {
public:
    enum class Direction { horizontal, vertical };

    explicit Split(const Direction direction = Direction::horizontal, const int ratio = 50)
        : direction_(direction), ratio_(std::clamp(ratio, 0, 100)) { set_focusable(false); }

    [[nodiscard]] auto hint() const -> SizeHint override {
        return detail::measure_box(*this, direction_ == Direction::horizontal);
    }

    void layout() override {
        const auto bounds = rect();
        if (children().size() < 2) return;
        const auto horizontal = direction_ == Direction::horizontal;
        const auto extent = std::max(0, horizontal ? bounds.width : bounds.height);
        const auto a = children()[0]->hint().min;
        const auto b = children()[1]->hint().min;
        const auto min_a = std::max(0, horizontal ? a.width : a.height);
        const auto min_b = std::max(0, horizontal ? b.width : b.height);
        const auto demand = static_cast<std::int64_t>(min_a) + min_b;
        const auto preferred = drag_offset_ >= 0 ? drag_offset_ :
            static_cast<int>(static_cast<std::int64_t>(extent) * ratio_ / 100);
        const auto first = demand > extent
            ? static_cast<int>(static_cast<std::int64_t>(extent) * min_a / demand)
            : std::clamp(preferred, min_a, extent - min_b);
        if (horizontal) {
            children()[0]->set_rect({bounds.x, bounds.y, first, bounds.height});
            children()[1]->set_rect({bounds.x + first, bounds.y, extent - first, bounds.height});
        } else {
            children()[0]->set_rect({bounds.x, bounds.y, bounds.width, first});
            children()[1]->set_rect({bounds.x, bounds.y + first, bounds.width, extent - first});
        }
    }

    void set_ratio(const int ratio) {
        ratio_ = std::clamp(ratio, 0, 100);
        drag_offset_ = -1;
        invalidate(rect());
    }
    [[nodiscard]] auto ratio() const noexcept -> int { return ratio_; }

    /// Moves the boundary to a cell offset measured from this split's origin.
    void drag_to(const int offset) {
        const auto extent = direction_ == Direction::horizontal ? rect().width : rect().height;
        drag_offset_ = std::clamp(offset, 0, std::max(0, extent));
        if (extent > 0) ratio_ = static_cast<int>(100LL * drag_offset_ / extent);
        invalidate(rect());
    }

    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* mouse = std::get_if<ttytk::input::Mouse>(&event);
        if (!mouse || children().size() < 2 || !mouse->x || !mouse->y ||
            !std::in_range<int>(mouse->x - 1) || !std::in_range<int>(mouse->y - 1)) return false;
        const auto horizontal = direction_ == Direction::horizontal;
        const auto x = static_cast<int>(mouse->x - 1);
        const auto y = static_cast<int>(mouse->y - 1);
        if (!rect().contains(x, y)) return false;
        if (!mouse->pressed) return std::exchange(dragging_, false);
        const auto offset = horizontal ? x - rect().x : y - rect().y;
        if (!mouse->motion) {
            const auto first = children()[0]->rect();
            dragging_ = mouse->button == 0 && offset == (horizontal ? first.width : first.height);
        } else if (dragging_) {
            drag_to(offset);
        }
        return dragging_;
    }

private:
    Direction direction_;
    int ratio_;
    int drag_offset_{-1};
    bool dragging_{};
};

}  // namespace ttyutils::ui
