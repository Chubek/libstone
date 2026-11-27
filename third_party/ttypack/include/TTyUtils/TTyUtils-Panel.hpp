#pragma once

#include <TTyUtils/TTyUtils-Layout.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ui {

enum class Border { None, Single, Double, Rounded, Heavy, Custom };
enum class TitlePos { Left, Center, Right };

struct Shadow {
    bool enabled{};
    int dx{1};
    int dy{1};
};

class Panel : public Widget {
public:
    using BorderGlyphs = std::array<std::string, 6>; // top-left, top-right, bottom-left, bottom-right, horizontal, vertical
    struct TitleSegment {
        std::string text;
        theme::Style style;
    };

    Panel() = default;
    explicit Panel(std::string title, const Border border = Border::Single)
        : title_(std::move(title)), border_(border) {}

    void set_child(std::unique_ptr<Widget> content) {
        (void)remove_child(child());
        if (content && collapsed_) {
            child_was_visible_ = content->visible();
            content->set_visible(false);
        }
        add_child(std::move(content));
    }
    void set_title(std::string title) {
        title_ = std::move(title);
        title_segments_.clear();
        invalidate(rect());
    }
    void set_title_segments(std::vector<TitleSegment> segments) {
        title_segments_ = std::move(segments);
        invalidate(rect());
    }
    void set_title_pos(const TitlePos position) { title_pos_ = position; invalidate(rect()); }
    void set_border(const Border border) { border_ = border; invalidate(rect()); }
    void set_border_glyphs(BorderGlyphs glyphs) {
        border_glyphs_ = std::move(glyphs);
        set_border(Border::Custom);
    }
    void set_shadow(const Shadow shadow) { shadow_ = shadow; invalidate(rect()); }
    void set_padding(const int cells) { padding_ = std::max(0, cells); invalidate(rect()); }
    void set_collapsed(const bool collapsed) {
        if (collapsed == collapsed_) return;
        collapsed_ = collapsed;
        if (child()) {
            if (collapsed) child_was_visible_ = child()->visible();
            child()->set_visible(!collapsed && child_was_visible_);
        }
        invalidate(rect());
    }
    [[nodiscard]] auto collapsed() const noexcept -> bool { return collapsed_; }
    [[nodiscard]] auto child() const noexcept -> Widget* { return children().empty() ? nullptr : children()[0].get(); }

    void layout() override {
        if (!child() || collapsed_) return;
        const auto area = frame();
        const auto inset = padding_ + static_cast<long long>(border_ != Border::None);
        const auto x = static_cast<int>(std::min<long long>(inset, area.width));
        const auto y = static_cast<int>(std::min<long long>(inset, area.height));
        child()->set_rect({area.x + x, area.y + y,
                          static_cast<int>(std::max(0LL, area.width - 2 * inset)),
                          static_cast<int>(std::max(0LL, area.height - 2 * inset))});
    }

    void paint(Canvas& canvas) override {
        const auto area = frame();
        if (area.empty()) return;
        if (shadow_.enabled) {
            const auto full = rect();
            const auto dx = std::clamp(shadow_.dx, -full.width, full.width);
            const auto dy = std::clamp(shadow_.dy, -full.height, full.height);
            auto shade = Rect{area.x + dx, area.y + dy, area.width, area.height}.intersect(canvas.clip());
            for (int row = 0; row < shade.height; ++row)
                for (int col = 0; col < shade.width; ++col)
                    if (!area.contains(shade.x + col, shade.y + row))
                        canvas.text(shade.x + col, shade.y + row, canvas.unicode() ? "░" : ".", style());
        }
        if (border_ == Border::None) return;
        const auto glyph = glyphs(canvas.unicode());
        const auto right = area.x + area.width - 1;
        const auto bottom = area.y + area.height - 1;
        auto draw = [&](const int x, const int y, const std::size_t index) {
            canvas.text(x, y, glyph[index], style());
        };
        draw(area.x, area.y, 0);
        if (area.width > 1) draw(right, area.y, 1);
        if (area.height > 1) {
            draw(area.x, bottom, 2);
            if (area.width > 1) draw(right, bottom, 3);
        }
        for (int x = area.x + 1; x < right; ++x) {
            draw(x, area.y, 4);
            if (area.height > 1) draw(x, bottom, 4);
        }
        for (int y = area.y + 1; y < bottom; ++y) {
            draw(area.x, y, 5);
            if (area.width > 1) draw(right, y, 5);
        }
        draw_title(canvas, area);
    }

private:
    [[nodiscard]] auto frame() const noexcept -> Rect {
        auto area = rect();
        if (area.empty()) return {};
        if (shadow_.enabled) {
            const auto dx = std::clamp(shadow_.dx, -area.width, area.width);
            const auto dy = std::clamp(shadow_.dy, -area.height, area.height);
            area.x -= std::min(0, dx);
            area.y -= std::min(0, dy);
            area.width -= std::abs(dx);
            area.height -= std::abs(dy);
        }
        if (collapsed_) area.height = std::min(1, area.height);
        return area;
    }

    [[nodiscard]] auto glyphs(const bool unicode) const -> std::array<std::string_view, 6> {
        if (!unicode) return {"+", "+", "+", "+", border_ == Border::Double ? "=" : "-", "|"};
        switch (border_) {
        case Border::Double: return {"╔", "╗", "╚", "╝", "═", "║"};
        case Border::Rounded: return {"╭", "╮", "╰", "╯", "─", "│"};
        case Border::Heavy: return {"┏", "┓", "┗", "┛", "━", "┃"};
        case Border::Custom:
            if (std::all_of(border_glyphs_.begin(), border_glyphs_.end(), [](const auto& glyph) {
                    return cell_width(glyph) == 1 && ttytk::grapheme::count(glyph) == 1;
                }))
                return {border_glyphs_[0], border_glyphs_[1], border_glyphs_[2],
                        border_glyphs_[3], border_glyphs_[4], border_glyphs_[5]};
            [[fallthrough]];
        default: return {"┌", "┐", "└", "┘", "─", "│"};
        }
    }

    void draw_title(Canvas& canvas, const Rect area) const {
        auto available = std::max(0, area.width - 2);
        if (available == 0) return;
        auto width = 0;
        if (title_segments_.empty()) width = cell_width(fit_text(title_, available));
        else for (const auto& segment : title_segments_) {
            const auto visible = fit_text(segment.text, available - width);
            width += cell_width(visible);
            if (visible.size() < segment.text.size()) break;
        }
        auto x = area.x + 1;
        if (title_pos_ == TitlePos::Center) x += (available - width) / 2;
        else if (title_pos_ == TitlePos::Right) x += available - width;
        if (title_segments_.empty()) canvas.text(x, area.y, fit_text(title_, width), style());
        else for (const auto& segment : title_segments_) {
            const auto visible = fit_text(segment.text, width);
            const auto cells = cell_width(visible);
            canvas.text(x, area.y, visible, segment.style);
            x += cells;
            width -= cells;
            if (visible.size() < segment.text.size()) break;
        }
    }

    std::string title_;
    std::vector<TitleSegment> title_segments_;
    BorderGlyphs border_glyphs_{};
    Border border_{Border::Single};
    TitlePos title_pos_{TitlePos::Left};
    Shadow shadow_{};
    int padding_{};
    bool collapsed_{};
    bool child_was_visible_{true};
};

}  // namespace ttyutils::ui
