#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-RectOps.hpp>
#include <TTyTk/TTyTk-TextLayout.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk::ui {

namespace detail {
[[nodiscard]] inline auto repeat(const std::string_view value, const std::size_t count) -> std::string {
    std::string output;
    for (std::size_t index = 0; index < count; ++index) output += value;
    return output;
}
}  // namespace detail

/** Line style for a box border. */
enum Border { SingleBorder, DoubleBorder };

class Box {
public:
    explicit Box(const Border border = SingleBorder) : border_(border) {}

    [[nodiscard]] inline auto draw(const std::size_t width, const std::size_t height) const
        -> std::vector<std::string> {
        if (width == 0 || height == 0) return {};
        const bool doubled = border_ == DoubleBorder;
        const std::string horizontal = doubled ? "\xe2\x95\x90" : "\xe2\x94\x80";
        const std::string vertical = doubled ? "\xe2\x95\x91" : "\xe2\x94\x82";
        const std::string top_left = doubled ? "\xe2\x95\x94" : "\xe2\x94\x8c";
        const std::string top_right = doubled ? "\xe2\x95\x97" : "\xe2\x94\x90";
        const std::string bottom_left = doubled ? "\xe2\x95\x9a" : "\xe2\x94\x94";
        const std::string bottom_right = doubled ? "\xe2\x95\x9d" : "\xe2\x94\x98";
        if (width == 1) return std::vector<std::string>(height, vertical);
        if (height == 1) return {detail::repeat(horizontal, width)};
        std::vector<std::string> lines;
        lines.reserve(height);
        lines.push_back(top_left + detail::repeat(horizontal, width - 2) + top_right);
        for (std::size_t row = 2; row < height; ++row)
            lines.push_back(vertical + std::string(width - 2, ' ') + vertical);
        lines.push_back(bottom_left + detail::repeat(horizontal, width - 2) + bottom_right);
        return lines;
    }

private:
    Border border_{};
};

class Label {
public:
    explicit Label(std::string text = {}) : text_(std::move(text)) {}
    [[nodiscard]] inline auto text() const noexcept -> std::string_view { return text_; }
    inline void set_text(std::string text) { text_ = std::move(text); }
    [[nodiscard]] inline auto draw(const std::size_t width) const -> std::string {
        return width == 0 ? std::string{} : layout::truncate(text_, width);
    }
private:
    std::string text_{};
};

class Layout {
public:
    /** A zero-based position and its extent in terminal cells. */
    struct Slot {
        std::size_t row{}, column{}, width{}, height{};
        friend constexpr auto operator==(const Slot&, const Slot&) -> bool = default;
    };

    inline void add(Label label) { labels_.push_back(std::move(label)); }
    [[nodiscard]] inline auto labels() const noexcept -> const std::vector<Label>& { return labels_; }

    /** Place labels in an equal-share row (or column), in insertion order. */
    [[nodiscard]] inline auto flex(const std::size_t width, const std::size_t height,
                                   const bool vertical = false) const -> std::vector<Slot> {
        return grid(width, height, vertical ? 1 : labels_.size());
    }

    /** Position labels row-major, distributing surplus cells to the first tracks. */
    [[nodiscard]] inline auto grid(const std::size_t width, const std::size_t height,
                                   const std::size_t columns) const -> std::vector<Slot> {
        if (labels_.empty() || columns == 0) return {};
        const auto rows = 1 + (labels_.size() - 1) / columns;
        std::vector<Slot> slots;
        slots.reserve(labels_.size());
        for (std::size_t i = 0; i < labels_.size(); ++i) {
            const auto row = i / columns;
            const auto column = i % columns;
            slots.push_back({
                row * (height / rows) + std::min(row, height % rows),
                column * (width / columns) + std::min(column, width % columns),
                width / columns + (column < width % columns),
                height / rows + (row < height % rows),
            });
        }
        return slots;
    }
private:
    std::vector<Label> labels_{};
};

class Panel {
public:
    Panel() = default;
    explicit Panel(Box box) : box_(std::move(box)) {}
    [[nodiscard]] inline auto box() const noexcept -> const Box& { return box_; }
    [[nodiscard]] inline auto draw(const std::size_t width, const std::size_t height) const
        -> std::vector<std::string> { return box_.draw(width, height); }
private:
    Box box_{};
};

struct Theme {
    color::Rgb foreground{255, 255, 255};
    color::Rgb background{};
};

}  // namespace ttytk::ui
