#pragma once

#include <TTyUtils/TTyUtils-Focus.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ttyutils::ui {

class Button : public Widget {
public:
    explicit Button(std::string label = {}) : label_(std::move(label)) {}
    void set_default(const bool value) noexcept { default_ = value; }
    void set_cancel(const bool value) noexcept { cancel_ = value; }
    void set_repeat(const bool value) noexcept { repeat_ = value; }
    [[nodiscard]] auto is_default() const noexcept -> bool { return default_; }
    [[nodiscard]] auto is_cancel() const noexcept -> bool { return cancel_; }
    void on_activate(std::function<void()> callback) { callback_ = std::move(callback); }
    void activate() {
        change_state();
        const auto callback = callback_;
        if (callback) callback();
    }
    [[nodiscard]] auto label() const noexcept -> const std::string& { return label_; }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (const auto* key = std::get_if<ttytk::input::Key>(&event)) {
            if (key->modifiers != 0 || (key->event_type != 1 && (key->event_type != 2 || !repeat_)))
                return false;
            if (key->codepoint != U' ' && key->codepoint != U'\r' && key->codepoint != U'\n' &&
                !(cancel_ && key->codepoint == U'\x1b')) return false;
            activate();
            return true;
        }
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event)) {
            if (mouse->button != 0 && !(armed_ && !mouse->pressed && mouse->button == 3)) return false;
            int x{}, y{};
            const auto inside = mouse_cell(*mouse, x, y) && rect().contains(x, y);
            if (mouse->motion) return armed_;
            if (mouse->pressed) {
                armed_ = inside;
                return inside;
            }
            const auto armed = std::exchange(armed_, false);
            if (armed && inside) activate();
            return armed;
        }
        return false;
    }

    void paint(Canvas& canvas) override {
        if (rect().width > 0 && rect().height > 0)
            canvas.text(rect().x, rect().y, prefix() + label_, style());
    }

protected:
    virtual void change_state() {}
    [[nodiscard]] virtual auto prefix() const -> std::string { return {}; }

private:
    std::string label_;
    std::function<void()> callback_{};
    bool default_{};
    bool cancel_{};
    bool repeat_{};
    bool armed_{};
};

class CheckBox : public Button {
public:
    enum class State { Unchecked, Checked, Indeterminate };
    explicit CheckBox(std::string label = {}) : Button(std::move(label)) {}
    void set_state(const State state) noexcept { state_ = state; }
    void set_tri_state(const bool value) noexcept { tri_state_ = value; }
    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    void toggle() noexcept {
        if (state_ == State::Unchecked) state_ = State::Checked;
        else if (state_ == State::Checked) state_ = tri_state_ ? State::Indeterminate : State::Unchecked;
        else state_ = tri_state_ ? State::Unchecked : State::Checked;
    }

private:
    void change_state() override { toggle(); }
    [[nodiscard]] auto prefix() const -> std::string override {
        return state_ == State::Checked ? "[x] " : state_ == State::Indeterminate ? "[-] " : "[ ] ";
    }
    State state_{State::Unchecked};
    bool tri_state_{};
};

class Radio;

class RadioGroup {
public:
    RadioGroup() = default;
    RadioGroup(const RadioGroup&) = delete;
    auto operator=(const RadioGroup&) -> RadioGroup& = delete;
    ~RadioGroup();
    void add(Radio* radio);
    void remove(Radio* radio) noexcept;
    void select(Radio* selected);
    [[nodiscard]] auto selected() const noexcept -> Radio* { return selected_; }

private:
    std::vector<Radio*> radios_;
    Radio* selected_{};
};

class Radio : public Button {
public:
    Radio(std::string label, RadioGroup& group) : Button(std::move(label)) { group.add(this); }
    Radio(const Radio&) = delete;
    auto operator=(const Radio&) -> Radio& = delete;
    ~Radio() override { if (group_) group_->remove(this); }
    void select() { if (group_) group_->select(this); }
    [[nodiscard]] auto selected() const noexcept -> bool { return selected_; }

private:
    friend class RadioGroup;
    void change_state() override { select(); }
    [[nodiscard]] auto prefix() const -> std::string override { return selected_ ? "(*) " : "( ) "; }
    RadioGroup* group_{};
    bool selected_{};
};

inline RadioGroup::~RadioGroup() {
    for (auto* radio : radios_) {
        radio->group_ = nullptr;
        radio->selected_ = false;
    }
}

inline void RadioGroup::add(Radio* radio) {
    if (!radio || radio->group_ == this) return;
    radios_.push_back(radio);
    if (radio->group_) radio->group_->remove(radio);
    radio->group_ = this;
}

inline void RadioGroup::remove(Radio* radio) noexcept {
    if (!radio || radio->group_ != this) return;
    if (selected_ == radio) selected_ = nullptr;
    std::erase(radios_, radio);
    radio->group_ = nullptr;
    radio->selected_ = false;
}

inline void RadioGroup::select(Radio* selected) {
    if (selected && selected->group_ != this) return;
    selected_ = selected;
    for (auto* radio : radios_) radio->selected_ = radio == selected;
}

class Toggle : public Button {
public:
    explicit Toggle(std::string label = {}) : Button(std::move(label)) {}
    void set(bool value) noexcept { value_ = value; }
    void toggle() noexcept { value_ = !value_; }
    [[nodiscard]] auto value() const noexcept -> bool { return value_; }

private:
    void change_state() override { toggle(); }
    [[nodiscard]] auto prefix() const -> std::string override { return value_ ? "[on] " : "[off] "; }
    bool value_{};
};

}  // namespace ttyutils::ui
