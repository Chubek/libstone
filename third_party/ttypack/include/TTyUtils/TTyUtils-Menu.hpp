#pragma once

#include <TTyUtils/TTyUtils-Focus.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ttyutils::ui {

struct Accel {
    std::string text{};
    Accel() = default;
    Accel(std::string value) : text(std::move(value)) {}
    Accel(const char* value) : text(value) {}
};

class Menu;

struct MenuItem {
    std::string label{};
    Accel accelerator{};
    std::function<void()> action{};
    bool separator{};
    bool checked{};
    bool checkable{};
    unsigned radio_group{};
    std::shared_ptr<Menu> submenu{};
};

class Menu : public Widget {
public:
    Menu() = default;
    explicit Menu(std::string title, std::vector<MenuItem> items = {})
        : title_(std::move(title)), items_(std::move(items)) {}
    Menu(Menu&& other) noexcept : Widget(), title_(std::move(other.title_)), items_(std::move(other.items_)) {}
    auto operator=(Menu&& other) noexcept -> Menu& {
        if (this != &other) {
            title_ = std::move(other.title_);
            items_ = std::move(other.items_);
        }
        return *this;
    }
    Menu(const Menu&) = delete;
    auto operator=(const Menu&) -> Menu& = delete;
    void add(MenuItem item) { items_.push_back(std::move(item)); }
    void open() { active_ = true; selected_.reset(); step(false); }
    void close() noexcept { active_ = false; submenu_.reset(); }
    [[nodiscard]] auto opened() const noexcept -> bool { return active_; }
    void activate(const std::size_t index) {
        if (index >= items_.size() || items_[index].separator) return;
        auto& item = items_[index];
        if (item.submenu && item.submenu.get() != this) {
            submenu_ = item.submenu;
            item.submenu->set_rect(rect());
            item.submenu->open();
            return;
        }
        const auto action = item.action;
        if (item.radio_group != 0) {
            for (auto& peer : items_) if (peer.radio_group == item.radio_group) peer.checked = &peer == &item;
        } else if (item.checkable) item.checked = !item.checked;
        close();
        if (action) action();
    }
    [[nodiscard]] auto title() const noexcept -> const std::string& { return title_; }
    [[nodiscard]] auto items() const noexcept -> const std::vector<MenuItem>& { return items_; }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (const auto sub = submenu_.lock(); sub && sub->opened()) return sub->handle(event);
        if (!active_) return false;
        if (const auto* key = std::get_if<ttytk::input::Key>(&event)) {
            if (key->event_type != 1) return false;
            if (key->modifiers == 0) {
                if (key->codepoint == U'\x1b') { close(); return true; }
                if (key->codepoint == 57352 || key->codepoint == 57353) {
                    step(key->codepoint == 57352);
                    return true;
                }
                if (key->codepoint == U'\r' || key->codepoint == U' ' || key->codepoint == 57351) {
                    if (selected_) activate(*selected_);
                    return true;
                }
            }
            for (std::size_t i = 0; i < items_.size(); ++i) {
                const auto& item = items_[i];
                if (item.separator) continue;
                const auto key_char = key->codepoint <= 127 ? static_cast<char>(key->codepoint) : '\0';
                if ((key->modifiers == 2 && mnemonic(item.label) == key_char && key_char != '\0') ||
                    (key->modifiers == 4 && key_char != '\0' && item.accelerator.text == std::string{"C-"} + key_char)) {
                    activate(i);
                    return true;
                }
                const auto label = display_label(item.label);
                if (key->modifiers == 0 && key_char >= '!' && !label.empty() && lower(label.front()) == lower(key_char)) {
                    selected_ = i;
                    return true;
                }
            }
        }
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event); mouse && mouse->button == 0 &&
            mouse->pressed && !mouse->motion) {
            int x{}, y{};
            if (mouse_cell(*mouse, x, y) && rect().contains(x, y))
                activate(static_cast<std::size_t>(y - rect().y));
            else close();
            return true;
        }
        return false;
    }

    void paint(Canvas& canvas) override {
        if (const auto sub = submenu_.lock(); sub && sub->opened()) { sub->paint(canvas); return; }
        if (!active_ || rect().empty()) return;
        const auto rows = std::min(items_.size(), static_cast<std::size_t>(rect().height));
        for (std::size_t i = 0; i < rows; ++i) {
            const auto y = rect().y + static_cast<int>(i);
            const auto& item = items_[i];
            if (item.separator) {
                canvas.text(rect().x, y, std::string(static_cast<std::size_t>(rect().width), '-'), style());
                continue;
            }
            const auto selected = selected_ && *selected_ == i;
            const auto label = std::string(selected ? "> " : "  ") +
                (item.checkable || item.radio_group != 0 ? (item.checked ? "[x] " : "[ ] ") : "") +
                display_label(item.label);
            const auto accel = fit_text(item.accelerator.text, std::max(0, rect().width - 3));
            const auto hint_width = cell_width(accel);
            canvas.text(rect().x, y, fit_text(label, rect().width - (hint_width ? hint_width + 1 : 0)), style());
            if (hint_width) canvas.text(rect().x + rect().width - hint_width, y, accel, style());
        }
    }

    [[nodiscard]] static auto display_label(std::string_view label) -> std::string {
        std::string result;
        for (std::size_t i = 0; i < label.size(); ++i) {
            if (label[i] == '&' && i + 1 < label.size()) {
                if (label[i + 1] == '&') result += label[++i];
            } else result += label[i];
        }
        return result;
    }
    [[nodiscard]] static auto mnemonic(std::string_view label) -> char {
        for (std::size_t i = 0; i + 1 < label.size(); ++i) {
            if (label[i] != '&') continue;
            if (label[i + 1] == '&') ++i;
            else return lower(label[i + 1]);
        }
        return '\0';
    }

private:
    [[nodiscard]] static auto lower(const char ch) noexcept -> char {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>('a' + ch - 'A') : ch;
    }
    void step(const bool backward) {
        if (items_.empty()) return;
        auto i = selected_.value_or(backward ? 0 : items_.size() - 1);
        for (std::size_t n = 0; n < items_.size(); ++n) {
            i = backward ? (i == 0 ? items_.size() - 1 : i - 1) : (i + 1) % items_.size();
            if (!items_[i].separator) { selected_ = i; return; }
        }
    }
    std::string title_;
    std::vector<MenuItem> items_;
    std::optional<std::size_t> selected_;
    std::weak_ptr<Menu> submenu_;
    bool active_{};
};

class MenuBar : public Widget {
public:
    MenuBar() = default;
    template <typename... Rest>
    explicit MenuBar(Menu&& first, Rest&&... rest) {
        add(std::move(first));
        (add(std::forward<Rest>(rest)), ...);
    }
    void add(Menu menu) { menus_.push_back(std::move(menu)); }
    [[nodiscard]] auto menus() const noexcept -> const std::vector<Menu>& { return menus_; }
    void open(const std::size_t index) {
        if (index >= menus_.size() || rect().height <= 1) return;
        if (active_) menus_[*active_].close();
        active_ = index;
        menus_[index].set_rect({rect().x, rect().y + 1, rect().width, rect().height - 1});
        menus_[index].open();
    }
    auto handle(const ttytk::input::Event& event) -> bool override {
        if (const auto* key = std::get_if<ttytk::input::Key>(&event);
            key && key->event_type == 1 && key->modifiers == 2 && key->codepoint <= 127) {
            const auto ch = key->codepoint >= 'A' && key->codepoint <= 'Z' ? key->codepoint + 32 : key->codepoint;
            for (std::size_t i = 0; i < menus_.size(); ++i)
                if (ch != 0 && Menu::mnemonic(menus_[i].title()) == static_cast<char>(ch)) { open(i); return true; }
        }
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event);
            mouse && mouse->button == 0 && mouse->pressed && !mouse->motion) {
            int x{}, y{};
            if (mouse_cell(*mouse, x, y) && y == rect().y && rect().contains(x, y)) {
                auto at = x - rect().x;
                for (std::size_t i = 0; i < menus_.size(); ++i) {
                    at -= cell_width(Menu::display_label(menus_[i].title())) + 2;
                    if (at < 0) { open(i); return true; }
                }
            }
        }
        return active_ && menus_[*active_].handle(event);
    }
    void paint(Canvas& canvas) override {
        auto x = rect().x;
        auto remaining = rect().width;
        for (const auto& menu : menus_) {
            if (remaining <= 0) break;
            const auto label = " " + Menu::display_label(menu.title()) + " ";
            const auto shown = fit_text(label, remaining);
            canvas.text(x, rect().y, shown, style());
            const auto width = cell_width(shown);
            x += width;
            remaining -= width;
        }
        if (active_) menus_[*active_].paint(canvas);
    }

private:
    std::vector<Menu> menus_;
    std::optional<std::size_t> active_;
};

class ContextMenu : public Menu {
public:
    using Menu::Menu;
};

}  // namespace ttyutils::ui
