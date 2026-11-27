#pragma once

#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <vector>

namespace ttyutils::ui {

/// Restricts navigation and input to a modal subtree until this guard leaves scope.
class FocusScope {
public:
    explicit FocusScope(Widget& root) : previous_(active_), root_(&root) { active_ = this; }
    FocusScope(const FocusScope&) = delete;
    auto operator=(const FocusScope&) -> FocusScope& = delete;
    ~FocusScope() {
        auto** link = &active_;
        while (*link && *link != this) link = &(*link)->previous_;
        if (*link) *link = previous_;
    }
    [[nodiscard]] auto root() const noexcept -> Widget* { return root_; }
    [[nodiscard]] static auto active() noexcept -> Widget* { return active_ ? active_->root_ : nullptr; }

    /// An unrelated widget tree is not affected by another application's modal scope.
    [[nodiscard]] static auto within(Widget& root) noexcept -> Widget& {
        auto* tree = &root;
        while (tree->parent()) tree = tree->parent();
        for (auto* scope = active_; scope; scope = scope->previous_) {
            auto* modal_tree = scope->root_;
            while (modal_tree->parent()) modal_tree = modal_tree->parent();
            if (modal_tree != tree) continue;
            for (auto* node = &root; node; node = node->parent())
                if (node == scope->root_) return root;
            return *scope->root_;
        }
        return root;
    }

private:
    inline static thread_local FocusScope* active_{};
    FocusScope* previous_;
    Widget* root_;
};

enum class FocusDirection { Left, Right, Up, Down };

/// Stable traversal by ascending tab order, then pre-order widget ownership.
class FocusChain {
public:
    FocusChain() = default;
    explicit FocusChain(Widget& root) : root_(&root) { rebuild(); }

    void rebuild() {
        widgets_.clear();
        if (root_) collect(FocusScope::within(*root_));
        std::stable_sort(widgets_.begin(), widgets_.end(), [](const auto* a, const auto* b) {
            return a->tab_order() < b->tab_order();
        });
    }

    /// Returns the snapshot from the last rebuild or traversal.
    [[nodiscard]] auto widgets() const noexcept -> const std::vector<Widget*>& { return widgets_; }
    [[nodiscard]] auto current() const -> Widget* {
        Widget* focused{};
        if (root_) find_focused(FocusScope::within(*root_), focused);
        return focused;
    }

    auto next() -> Widget* { return step(false); }
    auto prev() -> Widget* { return step(true); }

    /// Selects a widget if it is eligible in the current scope.
    auto focus(Widget& target) -> Widget* {
        rebuild();
        if (std::find(widgets_.begin(), widgets_.end(), &target) == widgets_.end()) return nullptr;
        return select(&target);
    }

    /// Selects the nearest eligible center in the requested half-plane.
    auto move(const FocusDirection direction) -> Widget* {
        rebuild();
        auto* origin = current();
        if (!origin) return widgets_.empty() ? nullptr : select(widgets_.front());
        const auto rect = origin->rect();
        auto score = std::tuple{0LL, 0LL};
        Widget* nearest = nullptr;
        for (auto* widget : widgets_) {
            if (widget == origin || widget->rect().empty()) continue;
            const auto candidate = widget->rect();
            const auto dx = 2LL * (static_cast<long long>(candidate.x) - rect.x) + candidate.width - rect.width;
            const auto dy = 2LL * (static_cast<long long>(candidate.y) - rect.y) + candidate.height - rect.height;
            const auto horizontal = direction == FocusDirection::Left || direction == FocusDirection::Right;
            const auto forward = horizontal ? dx : dy;
            if ((direction == FocusDirection::Left || direction == FocusDirection::Up)
                  ? forward >= 0 : forward <= 0) continue;
            const auto rank = std::tuple{std::abs(dx) + std::abs(dy), std::abs(horizontal ? dy : dx)};
            if (!nearest || rank < score) {
                score = rank;
                nearest = widget;
            }
        }
        return nearest ? select(nearest) : origin;
    }

    void set_root(Widget& root) { root_ = &root; rebuild(); }

private:
    auto step(const bool backward) -> Widget* {
        rebuild();
        if (widgets_.empty()) return nullptr;
        const auto it = std::find(widgets_.begin(), widgets_.end(), current());
        if (backward) return select(it == widgets_.begin() || it == widgets_.end()
                                    ? widgets_.back() : *std::prev(it));
        return select(it == widgets_.end() || std::next(it) == widgets_.end()
                      ? widgets_.front() : *std::next(it));
    }

    auto select(Widget* target) -> Widget* {
        auto* tree = root_;
        while (tree->parent()) tree = tree->parent();
        deselect_others(*tree, target);
        target->set_focused(true);
        return target;
    }

    static void deselect_others(Widget& widget, const Widget* except) {
        if (&widget != except) widget.set_focused(false);
        for (const auto& child : widget.children()) deselect_others(*child, except);
    }

    static void find_focused(Widget& widget, Widget*& focused) {
        if (!widget.visible()) return;
        if (widget.focused() && widget.focusable() && widget.hint().stretch >= 0 &&
            (!focused || widget.tab_order() < focused->tab_order())) focused = &widget;
        for (const auto& child : widget.children()) find_focused(*child, focused);
    }

    void collect(Widget& widget) {
        if (!widget.visible()) return;
        if (widget.focusable() && widget.hint().stretch >= 0) widgets_.push_back(&widget);
        for (const auto& child : widget.children()) collect(*child);
    }

    Widget* root_{};
    std::vector<Widget*> widgets_;
};

inline auto focus_next(Widget& root) -> Widget* { return FocusChain(root).next(); }
inline auto focus_prev(Widget& root) -> Widget* { return FocusChain(root).prev(); }

inline void on_focus(Widget& widget, std::function<void(bool)> callback) {
    widget.on_focus(std::move(callback));
}

}  // namespace ttyutils::ui
