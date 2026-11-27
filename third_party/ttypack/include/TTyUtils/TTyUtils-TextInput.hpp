#pragma once

#include <TTyUtils/TTyUtils-Focus.hpp>
#include <TTyUtils/TTyUtils-Readline.hpp>

#include <functional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace ttyutils::ui {

class Validator {
public:
    Validator() = default;
    explicit Validator(std::function<bool(std::string_view)> function) : function_(std::move(function)) {}
    /// Match the whole value. A malformed pattern yields a rejecting validator.
    [[nodiscard]] static auto regex(const std::string& pattern) -> Validator {
        try {
            return Validator{[expression = std::regex(pattern)](const std::string_view text) {
                try { return std::regex_match(text.begin(), text.end(), expression); }
                catch (const std::regex_error&) { return false; }
            }};
        } catch (const std::regex_error&) {
            return Validator{[](std::string_view) { return false; }};
        }
    }
    [[nodiscard]] auto valid(const std::string_view text) const -> bool {
        return !function_ || function_(text);
    }

private:
    std::function<bool(std::string_view)> function_{};
};

struct InputMask {
    std::string pattern{};
    /// `9` accepts an ASCII digit; every other byte is a literal separator.
    [[nodiscard]] auto accepts(const std::string_view value) const -> bool {
        if (pattern.empty()) return true;
        if (value.size() > pattern.size()) return false;
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (pattern[index] == '9') {
                if (value[index] < '0' || value[index] > '9') return false;
            } else if (pattern[index] != value[index]) return false;
        }
        return true;
    }
};

class TextInput : public Widget {
public:
    explicit TextInput(std::string placeholder = {}) : placeholder_(std::move(placeholder)) {
        editor_.set_multiline(false);
    }
    void set_text(std::string text) {
        if (mask_.accepts(text) && is_single_line(text)) {
            editor_.set_value(std::move(text));
            invalidate(rect());
        }
    }
    [[nodiscard]] auto text() const noexcept -> const std::string& { return editor_.text(); }
    void set_placeholder(std::string value) { placeholder_ = std::move(value); invalidate(rect()); }
    void set_validator(Validator validator) { validator_ = std::move(validator); invalidate(rect()); }
    void set_mask(InputMask mask) { mask_ = std::move(mask); invalidate(rect()); }
    void set_error_style(theme::Style value) { error_style_ = std::move(value); invalidate(rect()); }
    void set_completer(std::function<std::vector<completion::Candidate>(std::string_view)> completer) {
        editor_.set_completer(std::move(completer));
    }
    [[nodiscard]] auto completions() const -> std::vector<completion::Candidate> { return editor_.complete(); }
    [[nodiscard]] auto valid() const -> bool {
        return mask_.accepts(text()) && (mask_.pattern.empty() || mask_.pattern.size() == text().size()) &&
               validator_.valid(text());
    }
    void on_submit(std::function<void(std::string)> callback) { submit_ = std::move(callback); }
    void submit() { if (valid() && submit_) submit_(text()); }

    /// Route an event from the owning event loop to this single-line editor.
    void feed(const ttytk::input::Event& event) {
        const auto before = text();
        editor_.feed(event);
        if (before != text() && (!mask_.accepts(text()) || !is_single_line(text()))) (void)editor_.undo();
        invalidate(rect());
        if (editor_.accepted()) {
            editor_.reset_acceptance();
            submit();
        }
    }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (const auto* key = std::get_if<ttytk::input::Key>(&event)) {
            if (key->event_type == 3) return false;
        } else if (!std::holds_alternative<ttytk::input::Paste>(event)) return false;
        feed(event);
        return true;
    }

    void paint(Canvas& canvas) override {
        paint_value(canvas, text().empty() ? std::string_view(placeholder_) : std::string_view(text()),
                    editor_.cursor());
    }

protected:
    [[nodiscard]] auto cursor_byte() const noexcept -> std::size_t { return editor_.cursor(); }
    void paint_value(Canvas& canvas, const std::string_view value, const std::size_t cursor) {
        const auto area = rect();
        if (area.width <= 0 || area.height <= 0) return;
        auto width = 0;
        std::size_t begin = 0;
        std::size_t end = 0;
        while (end < value.size()) {
            const auto next = ttytk::grapheme::next_break(value, end);
            const auto cells = ttytk::charwidth::of_grapheme(value.substr(end, next - end));
            if (width + cells > area.width && end >= cursor) break;
            width += cells;
            end = next;
            while (width > area.width && begin < end) {
                const auto skip = ttytk::grapheme::next_break(value, begin);
                width -= ttytk::charwidth::of_grapheme(value.substr(begin, skip - begin));
                begin = skip;
            }
        }
        canvas.text(area.x, area.y, value.substr(begin, end - begin),
                    error_style_ && !valid() ? *error_style_ : style());
    }

private:
    static auto is_single_line(const std::string_view value) -> bool {
        if (!ttytk::utf8::validate(value)) return false;
        for (const auto cp : ttytk::utf8::decode(value))
            if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return false;
        return true;
    }

    rl::Editor editor_;
    std::string placeholder_;
    Validator validator_;
    InputMask mask_;
    std::optional<theme::Style> error_style_;
    std::function<void(std::string)> submit_{};
};

class PasswordInput : public TextInput {
public:
    using TextInput::TextInput;
    void set_reveal_last(const bool reveal) { reveal_last_ = reveal; invalidate(rect()); }
    [[nodiscard]] auto display_text() const -> std::string {
        const auto pieces = ttytk::grapheme::segment(text());
        if (reveal_last_ && !pieces.empty()) return std::string(pieces.size() - 1, '*') + std::string(pieces.back());
        return std::string(pieces.size(), '*');
    }
    void paint(Canvas& canvas) override {
        const auto display = display_text();
        const auto at = cursor_byte() == text().size() ? display.size() :
            ttytk::grapheme::count(std::string_view(text()).substr(0, cursor_byte()));
        paint_value(canvas, display, at);
    }

private:
    bool reveal_last_{};
};

}  // namespace ttyutils::ui
