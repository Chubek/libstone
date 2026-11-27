#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-KeyListener.hpp>
#include <TTyTk/TTyTk-SearchTools.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk {

/** Synchronous, UTF-8 line editor with a byte-indexed grapheme cursor. */
class Editor {
public:
    using CompletionCallback = std::function<std::vector<std::string>(std::string_view)>;

    inline void insert(const char32_t character) {
        insert_text(utf8::encode(character));
    }

    inline void backspace() {
        if (cursor_ == 0) return;
        const auto previous = previous_break();
        line_.erase(previous, cursor_ - previous);
        cursor_ = previous;
        normalize_cursor();
    }

    /** Move within the current editable text, including embedded newlines. */
    inline void move_left() { cursor_ = previous_break(); }
    inline void move_right() { cursor_ = grapheme::next_break(line_, cursor_); }

    /** Kill through the end of the current line, or kill the newline at its end. */
    inline void kill_to_end() {
        const auto newline = line_.find('\n', cursor_);
        const auto end = newline == std::string::npos ? line_.size() :
                         newline == cursor_ ? newline + 1 : newline;
        if (end == cursor_) return;
        kill_ring_.push_back(line_.substr(cursor_, end - cursor_));
        if (kill_ring_.size() > 32) kill_ring_.erase(kill_ring_.begin());
        line_.erase(cursor_, end - cursor_);
        normalize_cursor();
    }

    /** Insert the latest kill without removing it from the ring. */
    inline void yank() {
        if (!kill_ring_.empty()) insert_text(kill_ring_.back());
    }

    [[nodiscard]] inline auto history_up() -> std::string_view {
        if (history_.empty()) return line_;
        if (history_index_ == history_.size()) {
            saved_line_ = line_;
            saved_cursor_ = cursor_;
        }
        if (history_index_ > 0) --history_index_;
        line_ = history_[history_index_];
        cursor_ = line_.size();
        return line_;
    }

    /** Advance through history and restore the draft when leaving history. */
    [[nodiscard]] inline auto history_down() -> std::string_view {
        if (history_index_ == history_.size()) return line_;
        ++history_index_;
        if (history_index_ == history_.size()) {
            line_ = std::move(saved_line_);
            cursor_ = saved_cursor_;
        } else {
            line_ = history_[history_index_];
            cursor_ = line_.size();
        }
        return line_;
    }

    inline void set_completion_callback(CompletionCallback callback) {
        completion_callback_ = callback ? std::make_shared<CompletionCallback>(std::move(callback)) : nullptr;
    }

    /** Request full-line candidates, replacing the line only for a unique valid match. */
    [[nodiscard]] inline auto complete() -> std::vector<std::string> {
        if (!completion_callback_) return {};
        const auto original = line_;
        const auto callback = completion_callback_;
        auto candidates = (*callback)(original);
        if (candidates.size() == 1 && utf8::validate(candidates.front()) && line_ == original) {
            line_ = candidates.front();
            cursor_ = line_.size();
        }
        return candidates;
    }

    inline void on_submit(std::function<void(std::string_view)> callback) {
        submit_callback_ = callback ? std::make_shared<decltype(callback)>(std::move(callback)) : nullptr;
    }

    inline void submit() {
        if (!line_.empty()) history_.push_back(line_);
        history_index_ = history_.size();
        auto submitted = std::exchange(line_, {});
        saved_line_.clear();
        cursor_ = 0;
        const auto callback = submit_callback_;
        if (callback) (*callback)(submitted);
    }

    [[nodiscard]] inline auto line() const noexcept -> std::string_view { return line_; }
    [[nodiscard]] inline auto cursor() const noexcept -> std::size_t { return cursor_; }

private:
    inline void insert_text(const std::string_view text) {
        line_.insert(cursor_, text);
        cursor_ += text.size();
        normalize_cursor();
    }

    inline void normalize_cursor() {
        if (cursor_ == line_.size()) return;
        std::size_t offset = 0;
        while (offset < cursor_) offset = grapheme::next_break(line_, offset);
        cursor_ = offset;
    }

    [[nodiscard]] inline auto previous_break() const -> std::size_t {
        std::size_t previous = 0;
        for (std::size_t offset = 0; offset < cursor_; offset = grapheme::next_break(line_, offset))
            previous = offset;
        return previous;
    }

    std::string line_{};
    std::string saved_line_{};
    std::vector<std::string> history_{};
    std::vector<std::string> kill_ring_{};
    std::size_t cursor_{};
    std::size_t saved_cursor_{};
    std::size_t history_index_{};
    std::shared_ptr<CompletionCallback> completion_callback_{};
    std::shared_ptr<std::function<void(std::string_view)>> submit_callback_{};
};

}  // namespace ttytk
