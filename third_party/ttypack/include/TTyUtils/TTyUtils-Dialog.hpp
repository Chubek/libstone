#pragma once

#include <TTyUtils/TTyUtils-Button.hpp>
#include <TTyUtils/TTyUtils-Panel.hpp>
#include <TTyUtils/TTyUtils-TextInput.hpp>

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ttyutils::ui {

enum class DialogResult { None, Yes, No, Ok, Cancel };

class Dialog : public Panel {
    struct Completion {
        DialogResult result{DialogResult::None};
        std::coroutine_handle<> waiter{};
    };

public:
    Dialog(std::string title = {}, std::string body = {})
        : Panel(std::move(title), Border::Rounded), body_(std::move(body)) { set_focusable(false); }
    [[nodiscard]] auto body() const noexcept -> const std::string& { return body_; }
    void set_body(std::string body) { body_ = std::move(body); invalidate(rect()); }
    void set_roles(const DialogResult accept, const DialogResult cancel) noexcept {
        accept_ = accept;
        cancel_ = cancel;
    }

    /** Show inside the caller's widget stack and restrict its focus to this dialog. */
    void open() {
        if (result() != DialogResult::None || focus_) return;
        set_visible(true);
        focus_.emplace(*this);
        (void)focus_next(*this);
    }
    void on_result(std::function<void(DialogResult)> callback) { result_callback_ = std::move(callback); }
    void accept() { finish(accept_); }
    void cancel() { finish(cancel_); }
    [[nodiscard]] auto result() const noexcept -> DialogResult { return completion_->result; }

    /** Install an editable single-line value. Read input_text() in the result callback. */
    void set_input(std::string initial) {
        auto input = std::make_unique<TextInput>();
        input->set_text(std::move(initial));
        input->on_submit([this](std::string) { accept(); });
        set_child(std::move(input));
    }
    [[nodiscard]] auto input_text() const -> std::optional<std::string> {
        if (const auto* input = dynamic_cast<const TextInput*>(child())) return input->text();
        return std::nullopt;
    }

    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* key = std::get_if<ttytk::input::Key>(&event);
        if (!key || key->event_type != 1 || key->modifiers != 0 || result() != DialogResult::None) return false;
        if (key->codepoint == U'\x1b') { cancel(); return true; }
        if (key->codepoint == U'\r' || key->codepoint == U'\n') { accept(); return true; }
        return false;
    }

    void layout() override {
        Panel::layout();
        if (auto* content = child(); content && !body_.empty() && content->rect().height > 0) {
            auto area = content->rect();
            ++area.y;
            --area.height;
            content->set_rect(area);
        }
    }

    void paint(Canvas& canvas) override {
        Panel::paint(canvas);
        if (rect().width > 2 && rect().height > 2)
            canvas.text(rect().x + 1, rect().y + 1, fit_text(body_, rect().width - 2), style());
    }

    /** A single coroutine may wait for this one-shot result. Keep the dialog alive until completion. */
    class Awaiter {
    public:
        Awaiter(const Awaiter&) = delete;
        auto operator=(const Awaiter&) -> Awaiter& = delete;
        ~Awaiter() { if (state_->waiter == registered_) state_->waiter = {}; }
        [[nodiscard]] auto await_ready() const noexcept -> bool { return state_->result != DialogResult::None; }
        auto await_suspend(std::coroutine_handle<> waiter) noexcept -> bool {
            if (state_->waiter) return false;
            registered_ = waiter;
            state_->waiter = waiter;
            return true;
        }
        [[nodiscard]] auto await_resume() const noexcept -> DialogResult { return state_->result; }

    private:
        friend class Dialog;
        explicit Awaiter(std::shared_ptr<Completion> state) : state_(std::move(state)) {}
        std::shared_ptr<Completion> state_;
        std::coroutine_handle<> registered_{};
    };
    [[nodiscard]] auto operator co_await() const -> Awaiter { return Awaiter{completion_}; }

private:
    void finish(const DialogResult value) {
        if (result() != DialogResult::None || value == DialogResult::None) return;
        const auto state = completion_;
        const auto callback = result_callback_;
        state->result = value;
        focus_.reset();
        set_visible(false);
        if (callback) callback(value);
        if (const auto awaiting = std::exchange(state->waiter, {})) awaiting.resume();
    }

    std::string body_;
    std::shared_ptr<Completion> completion_{std::make_shared<Completion>()};
    std::function<void(DialogResult)> result_callback_;
    std::optional<FocusScope> focus_;
    DialogResult accept_{DialogResult::Ok};
    DialogResult cancel_{DialogResult::Cancel};
};

/** Construct a message box; attach it to the caller's overlay and call open(). */
[[nodiscard]] inline auto message_box(const std::string_view title, const std::string_view message)
    -> std::unique_ptr<Dialog> {
    return std::make_unique<Dialog>(std::string(title), std::string(message));
}

[[nodiscard]] inline auto confirm(const std::string_view title, const std::string_view message)
    -> std::unique_ptr<Dialog> {
    auto dialog = message_box(title, message);
    dialog->set_roles(DialogResult::Yes, DialogResult::No);
    return dialog;
}

[[nodiscard]] inline auto prompt(const std::string_view title, const std::string_view message,
                                 std::string initial = {}) -> std::unique_ptr<Dialog> {
    auto dialog = message_box(title, message);
    dialog->set_input(std::move(initial));
    return dialog;
}

}  // namespace ttyutils::ui
