#pragma once

#include <TTyUtils/TTyUtils-ScrollBar.hpp>
#include <TTyUtils/TTyUtils-Search.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::ui {

class ListModel {
public:
    virtual ~ListModel() = default;
    [[nodiscard]] virtual auto size() const -> std::size_t = 0;
    [[nodiscard]] virtual auto text(std::size_t row) const -> std::string = 0;
};

using ListDelegate = std::function<void(Canvas&, std::size_t, Rect, std::string_view)>;

class MultiSelect {
public:
    void toggle(const std::size_t row) {
        if (selected_.contains(row)) selected_.erase(row);
        else selected_.insert(row);
    }
    void clear() { selected_.clear(); }
    [[nodiscard]] auto contains(const std::size_t row) const -> bool { return selected_.contains(row); }
    [[nodiscard]] auto rows() const noexcept -> const std::set<std::size_t>& { return selected_; }

private:
    std::set<std::size_t> selected_;
};

class ListView : public Widget, public Scrollable {
public:
    using Filter = std::function<bool(std::string_view, std::string_view)>;

    explicit ListView(std::shared_ptr<ListModel> model = {}) : model_(std::move(model)) {}
    void set_model(std::shared_ptr<ListModel> model) {
        model_ = std::move(model);
        multi_.clear();
        refresh();
    }
    void set_delegate(ListDelegate delegate) { delegate_ = std::move(delegate); }
    void set_filter(std::string value) { filter_ = std::move(value); refresh(); }
    /** Override substring matching, e.g. with a fuzzy matcher. */
    void set_filter_matcher(Filter matcher) { matcher_ = std::move(matcher); refresh(); }
    /** Rebuild filtered indices after model changes; unfiltered rows need no projection. */
    void refresh() {
        matches_.clear();
        if (model_ && !filter_.empty()) {
            const auto count = model_->size();
            for (std::size_t row = 0; row < count; ++row) {
                const auto text = model_->text(row);
                if (matcher_ ? matcher_(text, filter_) : text.find(filter_) != std::string::npos)
                    matches_.push_back(row);
            }
        }
        offset_ = selected_ = 0;
    }
    void on_choose(std::function<void(int)> callback) { choose_ = std::move(callback); }
    void choose() {
        if (length() == 0) return;
        const auto row = selected();
        if (row <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            const auto callback = choose_;
            if (callback) callback(static_cast<int>(row));
        }
    }
    /** Selected model index. With no rows this returns zero; choose() is a no-op. */
    [[nodiscard]] auto selected() const -> std::size_t {
        const auto count = length();
        return count == 0 ? 0 : row_at(std::min(selected_, count - 1));
    }
    void select(const std::size_t row) {
        if (filter_.empty()) select_index(row);
        else if (const auto found = std::lower_bound(matches_.begin(), matches_.end(), row);
                 found != matches_.end() && *found == row)
            select_index(static_cast<std::size_t>(found - matches_.begin()));
    }

    void select_index(const std::size_t index) {
        const auto count = length();
        if (count == 0) return;
        selected_ = std::min(index, count - 1);
        const auto first = offset();
        const auto page = std::max(visible_length(), std::size_t{1});
        if (selected_ < first) scroll_to(selected_);
        else if (selected_ - first >= page) scroll_to(selected_ - page + 1);
    }
    void move_selection(const int delta) {
        const auto count = length();
        if (count == 0) return;
        const auto current = std::min(selected_, count - 1);
        if (delta < 0) select_index(current - std::min(current, static_cast<std::size_t>(-(delta + 1)) + 1));
        else select_index(current + std::min(count - 1 - current, static_cast<std::size_t>(delta)));
    }
    /** Search visible-projection rows, starting after the current selection and wrapping. */
    auto type_ahead(const std::string_view prefix) -> bool {
        const auto count = length();
        if (prefix.empty() || count == 0) return false;
        auto index = std::min(selected_, count - 1);
        for (std::size_t remaining = count; remaining > 0; --remaining) {
            index = index == count - 1 ? 0 : index + 1;
            if (model_->text(row_at(index)).starts_with(prefix)) {
                select_index(index);
                return true;
            }
        }
        return false;
    }
    void toggle_selected() { if (length() != 0) multi_.toggle(selected()); }
    [[nodiscard]] auto selection() noexcept -> MultiSelect& { return multi_; }
    [[nodiscard]] auto selection() const noexcept -> const MultiSelect& { return multi_; }
    [[nodiscard]] auto model() const noexcept -> const std::shared_ptr<ListModel>& { return model_; }

    [[nodiscard]] auto length() const -> std::size_t override {
        return !model_ ? 0 : filter_.empty() ? model_->size() : matches_.size();
    }
    [[nodiscard]] auto visible_length() const -> std::size_t override {
        return static_cast<std::size_t>(std::max(0, rect().height));
    }
    [[nodiscard]] auto offset() const -> std::size_t override { return std::min(offset_, max_offset()); }
    void scroll_to(const std::size_t index) override { offset_ = std::min(index, max_offset()); }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event)) {
            int x{}, y{};
            if (length() == 0 || !mouse->pressed || mouse->motion ||
                !mouse_cell(*mouse, x, y) || !rect().contains(x, y)) return false;
            if (mouse->button == 64 || mouse->button == 65) {
                move_selection(mouse->button == 64 ? -3 : 3);
                return true;
            }
            if (mouse->button != 0) return false;
            const auto first = offset();
            const auto relative = static_cast<std::size_t>(y - rect().y);
            if (relative >= length() - first) return false;
            select_index(first + relative);
            return true;
        }
        const auto* key = std::get_if<ttytk::input::Key>(&event);
        if (!key || key->event_type == 3 || length() == 0) return false;
        const auto code = key->codepoint;
        if ((key->modifiers == 4 && code == U'n') || (key->modifiers == 0 && (code == 57353 || code == U'\x0e')))
            move_selection(1);
        else if ((key->modifiers == 4 && code == U'p') || (key->modifiers == 0 && (code == 57352 || code == U'\x10')))
            move_selection(-1);
        else if (key->modifiers != 0) return false;
        else if (code == 57356) select_index(0);
        else if (code == 57357) select_index(length() - 1);
        else if (code == 57354) move_selection(-std::max(1, rect().height));
        else if (code == 57355) move_selection(std::max(1, rect().height));
        else if (code == U'\r' || code == U'\n') { if (key->event_type == 1) choose(); }
        else if (code == U' ') { if (key->event_type == 1) toggle_selected(); }
        else if (code >= U'!' && code <= U'~') {
            const auto byte = static_cast<char>(code);
            return type_ahead(std::string_view{&byte, 1});
        } else return false;
        return true;
    }

    void paint(Canvas& canvas) override {
        if (!model_ || rect().width <= 0) return;
        const auto first = offset();
        const auto visible = std::min(visible_length(), length() - first);
        for (std::size_t i = 0; i < visible; ++i) {
            const auto row = row_at(first + i);
            const auto text = model_->text(row);
            const auto row_rect = Rect{rect().x, rect().y + static_cast<int>(i), rect().width, 1};
            if (delegate_) delegate_(canvas, row, row_rect, text);
            else canvas.text(row_rect.x, row_rect.y, fit_text(text, row_rect.width), style());
        }
    }

private:
    [[nodiscard]] auto row_at(const std::size_t index) const noexcept -> std::size_t {
        return filter_.empty() ? index : matches_[index];
    }
    std::shared_ptr<ListModel> model_;
    ListDelegate delegate_{};
    std::function<void(int)> choose_{};
    Filter matcher_{};
    std::string filter_;
    std::vector<std::size_t> matches_;
    MultiSelect multi_;
    std::size_t offset_{};
    std::size_t selected_{};
};

}  // namespace ttyutils::ui
