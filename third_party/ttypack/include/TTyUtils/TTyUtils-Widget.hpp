#pragma once

#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>
#include <TTyUtils/TTyUtils-Theme.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ui {

struct Rect {
    int x{};
    int y{};
    int width{};
    int height{};

    [[nodiscard]] constexpr auto contains(const int px, const int py) const noexcept -> bool {
        return px >= x && py >= y && static_cast<std::int64_t>(px) - x < width &&
               static_cast<std::int64_t>(py) - y < height;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool { return width <= 0 || height <= 0; }
    [[nodiscard]] friend constexpr auto operator==(const Rect&, const Rect&) noexcept -> bool = default;

    [[nodiscard]] constexpr auto intersect(const Rect other) const noexcept -> Rect {
        const auto left = std::max(x, other.x);
        const auto top = std::max(y, other.y);
        const auto right = std::min(static_cast<std::int64_t>(x) + width,
                                    static_cast<std::int64_t>(other.x) + other.width);
        const auto bottom = std::min(static_cast<std::int64_t>(y) + height,
                                     static_cast<std::int64_t>(other.y) + other.height);
        return {left, top, static_cast<int>(std::max<std::int64_t>(0, right - left)),
                           static_cast<int>(std::max<std::int64_t>(0, bottom - top))};
    }
};

struct SizeHint {
    struct Size {
        int width{};
        int height{};
    };
    Size min{};
    Size preferred{};
    int stretch{1};
};

namespace detail {

[[nodiscard]] inline auto cluster_width(const std::string_view cluster) noexcept -> int {
    return ttytk::charwidth::of_grapheme(cluster);
}

}  // namespace detail

/// Measures text in terminal cells without counting combining marks separately.
[[nodiscard]] inline auto cell_width(const std::string_view text) noexcept -> int {
    int width = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto end = ttytk::grapheme::next_break(text, offset);
        const auto cells = detail::cluster_width(text.substr(offset, end - offset));
        if (cells > std::numeric_limits<int>::max() - width) return std::numeric_limits<int>::max();
        width += cells;
        offset = end;
    }
    return width;
}

/// Returns the longest prefix that fits the given cell width, on a grapheme boundary.
[[nodiscard]] inline auto fit_text(const std::string_view text, int width) noexcept -> std::string_view {
    if (width <= 0) return {};
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto end = ttytk::grapheme::next_break(text, offset);
        const auto cells = detail::cluster_width(text.substr(offset, end - offset));
        if (cells > width) break;
        width -= cells;
        offset = end;
    }
    return text.substr(0, offset);
}

/// Converts positive wire coordinates to zero-based canvas coordinates.
[[nodiscard]] inline auto mouse_cell(const ttytk::input::Mouse& mouse, int& x, int& y) noexcept -> bool {
    if (!mouse.x || !mouse.y || !std::in_range<int>(mouse.x - 1) || !std::in_range<int>(mouse.y - 1)) return false;
    x = static_cast<int>(mouse.x - 1);
    y = static_cast<int>(mouse.y - 1);
    return true;
}

class Canvas {
public:
    struct Text {
        int x{};
        int y{};
        std::string value{};
        theme::Style style{};
    };

    explicit Canvas(const Rect clip = {}, const bool unicode = true) : clip_(clip), unicode_(unicode) {}

    /// Draws single-line text at absolute cell coordinates, without splitting
    /// grapheme clusters or writing partial wide cells at a clipping edge.
    void text(const int x, const int y, const std::string_view value, const theme::Style& style = {}) {
        if (!clip_.contains(clip_.x, y) || value.empty()) return;
        const auto right = static_cast<std::int64_t>(clip_.x) + clip_.width;
        auto column = static_cast<std::int64_t>(x);
        int start = 0;
        std::string clipped;
        for (std::size_t offset = 0; offset < value.size() && column < right;) {
            const auto end = ttytk::grapheme::next_break(value, offset);
            const auto cluster = value.substr(offset, end - offset);
            offset = end;
            // Terminal controls are not drawing commands.
            const auto first = ttytk::detail::decode_utf8_scalar(cluster, 0);
            if (first.value == U'\r' || first.value == U'\n') break;
            if (first.value < U' ' || (first.value >= 0x7f && first.value <= 0x9f)) continue;
            const auto width = detail::cluster_width(cluster);
            if (width == 0) {
                if (!clipped.empty()) clipped += cluster;
                continue;
            }
            if (column >= clip_.x && column + width <= right) {
                if (clipped.empty()) start = static_cast<int>(column);
                if (first.valid && first.length == cluster.size()) clipped += cluster;
                else {
                    for (std::size_t byte = 0; byte < cluster.size();) {
                        const auto scalar = ttytk::detail::decode_utf8_scalar(cluster, byte);
                        if (!scalar.valid) clipped += "\xef\xbf\xbd";
                        else if (scalar.value >= U' ' &&
                                 (scalar.value < 0x7f || scalar.value > 0x9f))
                            clipped += cluster.substr(byte, scalar.length);
                        byte += scalar.length;
                    }
                }
            }
            column += width;
        }
        if (!clipped.empty()) texts_.push_back({start, y, std::move(clipped), style});
    }
    [[nodiscard]] auto texts() const noexcept -> const std::vector<Text>& { return texts_; }
    [[nodiscard]] auto clip() const noexcept -> Rect { return clip_; }
    [[nodiscard]] auto unicode() const noexcept -> bool { return unicode_; }

private:
    friend class Widget;
    Rect clip_;
    bool unicode_;
    std::vector<Text> texts_;
};

class Widget;
struct HitTest {
    Widget* widget{};
};
using DamageList = std::vector<Rect>;

class Widget {
public:
    virtual ~Widget() = default;

    virtual void paint(Canvas&) {}
    /// Returns true when this widget consumed the terminal input event.
    virtual auto handle(const ttytk::input::Event&) -> bool { return false; }
    /// Computes child geometry before painting; layout may also be invoked explicitly.
    virtual void layout() {}
    [[nodiscard]] virtual auto hint() const -> SizeHint { return {}; }

    void set_rect(const Rect rect) {
        if (rect_ == rect) return;
        if (parent_) parent_->invalidate(rect_);
        rect_ = rect;
        invalidate(rect_);
    }
    [[nodiscard]] auto rect() const noexcept -> Rect { return rect_; }
    void set_style(theme::Style style) {
        if (style_ == style) return;
        style_ = std::move(style);
        invalidate(rect_);
    }
    [[nodiscard]] auto style() const noexcept -> const theme::Style& { return style_; }

    void add_child(std::unique_ptr<Widget> child) {
        if (child) {
            children_.push_back(std::move(child));
            children_.back()->parent_ = this;
            invalidate(rect_);
        }
    }
    /// Detaches a direct child and returns its ownership.
    [[nodiscard]] auto remove_child(Widget* child) -> std::unique_ptr<Widget> {
        const auto match = std::find_if(children_.begin(), children_.end(),
                                        [child](const auto& entry) { return entry.get() == child; });
        if (match == children_.end()) return {};
        invalidate((*match)->rect());
        auto result = std::move(*match);
        children_.erase(match);
        result->parent_ = nullptr;
        return result;
    }
    [[nodiscard]] auto children() const noexcept -> const std::vector<std::unique_ptr<Widget>>& { return children_; }
    [[nodiscard]] auto parent() const noexcept -> Widget* { return parent_; }

    void render(Canvas& canvas) {
        if (!visible_) return;
        const auto parent_clip = canvas.clip_;
        canvas.clip_ = parent_clip.intersect(rect_);
        // Later commands (including children) cover earlier commands.
        damage_.clear();
        try {
            if (!canvas.clip_.empty()) {
                layout();
                paint(canvas);
                for (auto& child : children_) child->render(canvas);
            }
        } catch (...) {
            canvas.clip_ = parent_clip;
            throw;
        }
        canvas.clip_ = parent_clip;
    }

    [[nodiscard]] auto hit_test(const int x, const int y) -> HitTest {
        if (!visible_ || !rect_.contains(x, y)) return {};
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            if (const auto hit = (*it)->hit_test(x, y); hit.widget != nullptr) return hit;
        }
        return HitTest{this};
    }

    void invalidate(const Rect rect) {
        const auto clipped = rect_.intersect(rect);
        if (clipped.empty()) return;
        if (std::find(damage_.begin(), damage_.end(), clipped) == damage_.end())
            damage_.push_back(clipped);
        if (parent_) parent_->invalidate(clipped);
    }
    [[nodiscard]] auto damage() const noexcept -> const DamageList& { return damage_; }
    void set_visible(const bool visible) {
        if (visible != visible_) {
            visible_ = visible;
            invalidate(rect_);
        }
    }
    [[nodiscard]] auto visible() const noexcept -> bool { return visible_; }
    void set_focusable(const bool value) noexcept { focusable_ = value; }
    [[nodiscard]] auto focusable() const noexcept -> bool { return focusable_; }
    void set_tab_order(const int order) noexcept { tab_order_ = order; }
    [[nodiscard]] auto tab_order() const noexcept -> int { return tab_order_; }
    void on_focus(std::function<void(bool)> callback) { focus_callback_ = std::move(callback); }
    void set_focused(const bool focused) {
        if (focused_ == focused) return;
        focused_ = focused;
        invalidate(rect_);
        if (focus_callback_) focus_callback_(focused);
    }
    [[nodiscard]] auto focused() const noexcept -> bool { return focused_; }

private:
    Rect rect_{};
    theme::Style style_{};
    Widget* parent_{};
    std::vector<std::unique_ptr<Widget>> children_;
    DamageList damage_;
    std::function<void(bool)> focus_callback_{};
    bool focused_{};
    bool visible_{true};
    bool focusable_{true};
    int tab_order_{};
};

}  // namespace ttyutils::ui
