#pragma once

#include <TTyUtils/TTyUtils-Menu.hpp>

#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ttyutils::ui {

struct Tab {
    std::string title{};
    bool modified{};
};

enum class TabPolicy { Scroll, Compress, PickList };

class TabBar : public Widget {
public:
    void add(Tab tab) { pick_.close(); tabs_.push_back(std::move(tab)); }
    void close(const std::size_t index) {
        if (index >= tabs_.size()) return;
        pick_.close();
        drag_.reset();
        const auto was_active = index == selected_;
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < selected_) --selected_;
        if (tabs_.empty()) { selected_ = 0; return; }
        if (was_active) select(std::min(selected_, tabs_.size() - 1));
    }
    void select(const std::size_t index) {
        if (index < tabs_.size()) {
            selected_ = index;
            const auto callback = callback_;
            if (callback && index <= static_cast<std::size_t>(std::numeric_limits<int>::max())) callback(static_cast<int>(index));
        }
    }
    void reorder(const std::size_t source, const std::size_t target) {
        if (source >= tabs_.size() || target >= tabs_.size() || source == target) return;
        pick_.close();
        const auto from = tabs_.begin() + static_cast<std::ptrdiff_t>(source);
        const auto to = tabs_.begin() + static_cast<std::ptrdiff_t>(target);
        if (source < target) std::rotate(from, from + 1, to + 1);
        else std::rotate(to, from, from + 1);
        if (selected_ == source) selected_ = target;
        else if (source < selected_ && selected_ <= target) --selected_;
        else if (target <= selected_ && selected_ < source) ++selected_;
    }
    void select_next() { if (!tabs_.empty()) select((selected_ + 1) % tabs_.size()); }
    void select_prev() { if (!tabs_.empty()) select(selected_ == 0 ? tabs_.size() - 1 : selected_ - 1); }
    void on_select(std::function<void(int)> callback) { callback_ = std::move(callback); }
    [[nodiscard]] auto selected() const noexcept -> std::size_t { return selected_; }
    [[nodiscard]] auto tabs() const noexcept -> const std::vector<Tab>& { return tabs_; }
    void set_policy(const TabPolicy policy) noexcept { policy_ = policy; }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (pick_.opened() && pick_.handle(event)) return true;
        if (const auto* key = std::get_if<ttytk::input::Key>(&event)) {
            if (key->event_type != 1 || tabs_.empty()) return false;
            if (key->modifiers == 4 && key->codepoint >= U'1' && key->codepoint <= U'9') {
                const auto target = static_cast<std::size_t>(key->codepoint - U'1');
                if (target >= tabs_.size()) return false;
                select(target);
                return true;
            }
            if (key->codepoint == U'\t' && (key->modifiers == 4 || key->modifiers == 5)) {
                if (key->modifiers == 4) select_next();
                else select_prev();
                return true;
            }
        }
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event)) {
            int x{}, y{};
            if (!mouse_cell(*mouse, x, y)) return false;
            if (!mouse->pressed && drag_) { drag_.reset(); return true; }
            if (mouse->button != 0 || !mouse->pressed || y != rect().y || !rect().contains(x, y)) return false;
            const auto items = placements();
            for (const auto& item : items) {
                if (x - rect().x < item.x || x - rect().x >= item.x + item.width) continue;
                if (!item.index) { if (!mouse->motion) open_pick(); return true; }
                const auto index = *item.index;
                if (mouse->motion) {
                    if (!drag_) return false;
                    reorder(*drag_, index);
                    drag_ = index;
                } else if (item.width >= 3 && x - rect().x == item.x + item.width - 1) close(index);
                else { drag_ = index; select(index); }
                return true;
            }
        }
        return false;
    }

    void paint(Canvas& canvas) override {
        for (const auto& item : placements()) {
            if (!item.index) {
                canvas.text(rect().x + item.x, rect().y, canvas.unicode() ? "…" : ".", style());
                continue;
            }
            const auto index = *item.index;
            const auto& tab = tabs_[index];
            const auto title = std::string(index == selected_ ? ">" : " ") +
                               (tab.modified ? (canvas.unicode() ? "•" : "*") : "") + tab.title;
            const auto location = rect().x + item.x;
            canvas.text(location, rect().y, fit_text(title, item.width - (item.width >= 3)), style());
            if (item.width >= 3) canvas.text(location + item.width - 1, rect().y, "x", style());
        }
        if (pick_.opened()) pick_.paint(canvas);
    }

private:
    struct Placement { std::optional<std::size_t> index; int x; int width; };
    [[nodiscard]] auto placements() const -> std::vector<Placement> {
        std::vector<Placement> result;
        if (tabs_.empty() || rect().empty()) return result;
        const auto desired = [this](const std::size_t i) {
            const auto consumed = static_cast<long long>(cell_width(tabs_[i].title)) + (tabs_[i].modified ? 4 : 3);
            return static_cast<int>(std::min<long long>(rect().width, consumed));
        };
        const auto strip_width = rect().width;
        long long sum = 0;
        for (std::size_t i = 0; i < tabs_.size() && sum <= strip_width; ++i) sum += desired(i);
        const auto overflow = sum > strip_width;
        const auto all_can_compress = policy_ == TabPolicy::Compress &&
                                      tabs_.size() <= static_cast<std::size_t>(strip_width);
        const auto pick = overflow && policy_ != TabPolicy::Scroll && !all_can_compress && strip_width > 1;
        const auto limit = strip_width - (pick ? 1 : 0);
        auto start = std::size_t{0};
        if (overflow && !all_can_compress) {
            start = selected_;
            auto used = std::min(limit, desired(start));
            while (start > 0 && desired(start - 1) <= limit - used) used += desired(--start);
        }
        auto x = 0;
        for (std::size_t i = start; i < tabs_.size() && x < limit; ++i) {
            const auto width = all_can_compress && overflow ? limit / static_cast<int>(tabs_.size()) :
                                                             std::min(limit - x, desired(i));
            result.push_back({i, x, width});
            x += width;
        }
        if (pick) result.push_back({std::nullopt, limit, 1});
        return result;
    }
    void open_pick() {
        if (rect().height <= 1) return;
        pick_ = Menu{"Tabs"};
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            pick_.add({tabs_[i].title, {}, [this, i] { select(i); }});
        pick_.set_rect({rect().x, rect().y + 1, rect().width, rect().height - 1});
        pick_.open();
    }
    std::vector<Tab> tabs_;
    std::size_t selected_{};
    TabPolicy policy_{TabPolicy::Scroll};
    std::function<void(int)> callback_{};
    std::optional<std::size_t> drag_;
    Menu pick_;
};

}  // namespace ttyutils::ui
