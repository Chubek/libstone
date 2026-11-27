#pragma once

#include <TTyTk/TTyTk-CharWidth.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>
#include <TTyUtils/TTyUtils-Completion.hpp>
#include <TTyUtils/TTyUtils-HistoryFile.hpp>
#include <TTyUtils/TTyUtils-KeyMap.hpp>
#include <TTyUtils/TTyUtils-TextBuffer.hpp>

#include <cstddef>
#include <algorithm>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ttyutils::rl {

struct Prompt {
    std::string left{};
    std::string right_text{};

    Prompt() = default;
    explicit Prompt(std::string value) : left(std::move(value)) {}
    [[nodiscard]] auto right(std::string value) && -> Prompt {
        right_text = std::move(value);
        return std::move(*this);
    }
};

struct Binding {
    std::string key{};
    std::string action{};
};

class Keymap {
public:
    void bind(Binding binding) { bindings_[std::move(binding.key)] = std::move(binding.action); }
    [[nodiscard]] auto action(const std::string_view key) const -> std::string_view {
        const auto found = bindings_.find(std::string(key));
        return found == bindings_.end() ? std::string_view{} : found->second;
    }

private:
    std::unordered_map<std::string, std::string> bindings_;
};

using KeymapAlias = Keymap;

class KillRing {
public:
    void push(std::string text) {
        if (text.empty()) return;
        ring_.push_front(std::move(text));
        if (ring_.size() > 32) ring_.pop_back();
        index_ = 0;
    }
    [[nodiscard]] auto yank() const -> std::string_view {
        return ring_.empty() ? std::string_view{} : ring_[index_ % ring_.size()];
    }
    [[nodiscard]] auto rotate() -> std::string_view {
        if (!ring_.empty()) index_ = (index_ + 1) % ring_.size();
        return yank();
    }

private:
    std::deque<std::string> ring_;
    std::size_t index_{};
};

class Editor {
public:
    enum class Mode { emacs, vi_insert, vi_normal };

    Editor() = default;

    void set_prompt(Prompt prompt) { prompt_ = std::move(prompt); }
    void set_completer(std::function<std::vector<completion::Candidate>(std::string_view)> completer) {
        completer_ = std::move(completer);
    }
    void set_history(hist::History* history) { history_ = history; history_pos_.reset(); }
    void set_multiline(const bool value) noexcept { multiline_ = value; }
    void set_mode(const Mode mode) noexcept { mode_ = mode; count_ = 0; }
    [[nodiscard]] auto keymap() noexcept -> Keymap& { return keymap_; }
    [[nodiscard]] auto cursor() const noexcept -> std::size_t { return cursor_; }

    /// Replace the line and reset transient editing state.
    void set_value(std::string value) {
        clear();
        text_ = std::move(value);
        cursor_ = text_.size();
    }

    /// Consume decoded input. Release events never edit the line.
    void feed(const ttytk::input::Event& event) {
        if (accepted_) return;
        if (const auto* key = std::get_if<ttytk::input::Key>(&event)) {
            if (key->event_type == 3) return; // Kitty progressive enhancement: release.
            auto cp = key->codepoint;
            constexpr unsigned ctrl = 4;
            if (key->modifiers & ctrl) {
                if (cp >= U'A' && cp <= U'Z') cp += U'a' - U'A';
                if (cp >= U'a' && cp <= U'z') cp -= U'a' - 1;
                else if (cp == U'_') cp = 31;
            }
            const auto binding = keymap_.action(key_name(cp));
            if (!binding.empty() && run(binding)) return;
            if (searching_) {
                if (cp == 18) { search_history(true); return; }
                if (cp == 7 || cp == 27) {
                    text_ = std::move(draft_);
                    cursor_ = text_.size();
                    searching_ = false;
                    history_pos_.reset();
                    return;
                }
                if (cp == 8 || cp == 127) {
                    search_.resize(previous(search_, search_.size()));
                    history_pos_.reset();
                    search_history(false);
                    return;
                }
                if (printable(cp) && !functional(cp) && !(key->modifiers & 0x3eU)) {
                    search_ += ttytk::utf8::encode(cp);
                    history_pos_.reset();
                    search_history(false);
                    return;
                }
                searching_ = false;
            }
            if (cp == U'\n' || cp == U'\r') {
                if (multiline_ && needs_continuation()) insert("\n");
                else accepted_ = true;
                return;
            }
            if (mode_ == Mode::vi_normal) {
                if (cp >= U'0' && cp <= U'9' && (count_ != 0 || cp != U'0')) {
                    count_ = std::min(std::size_t{100000}, count_ * 10 + static_cast<std::size_t>(cp - U'0'));
                    return;
                }
                const auto count = std::exchange(count_, 0);
                switch (cp) {
                case U'h': move(false, count); return;
                case U'l': move(true, count); return;
                case U'0': cursor_ = 0; return;
                case U'$': cursor_ = text_.size(); return;
                case U'i': mode_ = Mode::vi_insert; return;
                case U'a': move(true, 1); mode_ = Mode::vi_insert; return;
                case U'x': {
                    auto end = cursor_;
                    for (auto i = std::max(count, std::size_t{1}); i && end < text_.size(); --i)
                        end = ttytk::grapheme::next_break(text_, end);
                    erase(cursor_, end);
                    return;
                }
                case U'u': (void)undo(); return;
                case U'p': insert(kill_.yank()); return;
                default: break;
                }
            }
            switch (cp) {
            case 1: case 57356: (void)run("home"); return;
            case 2: case 57350: (void)run("left"); return;
            case 4: case 57349: (void)run("delete"); return;
            case 5: case 57357: (void)run("end"); return;
            case 6: case 57351: (void)run("right"); return;
            case 8: case 127: (void)run("backspace"); return;
            case 9: (void)run("complete"); return;
            case 11: (void)run("kill-end"); return;
            case 14: case 57353: (void)run("next-history"); return;
            case 16: case 57352: (void)run("previous-history"); return;
            case 18:
                if (history_) {
                    draft_ = text_;
                    search_.clear();
                    searching_ = true;
                    history_pos_.reset();
                    search_history(false);
                }
                return;
            case 21: (void)run("kill-start"); return;
            case 23: (void)run("kill-word"); return;
            case 25: (void)run("yank"); return;
            case 27: if (mode_ == Mode::vi_insert) mode_ = Mode::vi_normal; return;
            case 31: (void)undo(); return;
            default:
                if (mode_ != Mode::vi_normal && !(key->modifiers & 0x3eU)) {
                    if (!key->text.empty()) {
                        std::string value;
                        for (const auto scalar : key->text)
                            if (printable(scalar)) value += ttytk::utf8::encode(scalar);
                        insert(value);
                    } else if (printable(cp) && !functional(cp)) {
                        insert(ttytk::utf8::encode(cp));
                    }
                }
            }
        } else if (const auto* paste = std::get_if<ttytk::input::Paste>(&event)) {
            // xterm bracketed paste: insert the payload as one edit, never replay it as keys.
            std::string sanitized;
            for (const auto cp : ttytk::utf8::decode(paste->text)) {
                if (cp == U'\n' || cp == U'\t') sanitized += multiline_ ? static_cast<char>(cp) : ' ';
                else if (printable(cp)) sanitized += ttytk::utf8::encode(cp);
            }
            insert(sanitized);
        }
    }

    [[nodiscard]] auto value() const noexcept -> std::string_view { return text_; }
    [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
    [[nodiscard]] auto prompt() const noexcept -> const Prompt& { return prompt_; }
    [[nodiscard]] auto accepted() const noexcept -> bool { return accepted_; }
    void reset_acceptance() noexcept { accepted_ = false; }
    void clear() {
        text_.clear(); cursor_ = 0; accepted_ = false; undo_.clear();
        history_pos_.reset(); searching_ = false; count_ = 0; draft_.clear(); search_.clear();
    }
    [[nodiscard]] auto undo() -> bool {
        if (undo_.empty()) return false;
        text_ = std::move(undo_.back().first);
        cursor_ = undo_.back().second;
        undo_.pop_back();
        return true;
    }
    [[nodiscard]] auto complete() const -> std::vector<completion::Candidate> {
        return completer_ ? completer_(std::string_view(text_).substr(0, cursor_)) :
                            std::vector<completion::Candidate>{};
    }
    [[nodiscard]] auto read() -> std::optional<std::string> {
        if (!accepted_) return std::nullopt;
        auto line = std::move(text_);
        if (history_) (void)history_->append({.text = line});
        clear();
        return line;
    }
    /// Plain prompt rendering for dumb terminals. The side prompt never overlaps the input.
    [[nodiscard]] auto render(const int columns) const -> std::string {
        auto line = prompt_.left + text_;
        if (prompt_.right_text.empty()) return line;
        const auto last = std::string_view(line).substr(line.find_last_of('\n') + 1);
        const auto gap = columns - ttytk::charwidth::of_string(last) -
                         ttytk::charwidth::of_string(prompt_.right_text);
        if (gap > 0) line += std::string(static_cast<std::size_t>(gap), ' ') + prompt_.right_text;
        return line;
    }

private:
    static auto printable(const char32_t cp) noexcept -> bool {
        return cp >= 0x20 && (cp < 0x7f || cp >= 0xa0) &&
               !(cp >= 0xd800 && cp <= 0xdfff) && cp <= 0x10ffff;
    }
    // Reserved functional and modifier key identities from the Kitty protocol.
    static auto functional(const char32_t cp) noexcept -> bool { return cp >= 57344 && cp <= 57454; }
    static auto previous(const std::string_view text, const std::size_t cursor) -> std::size_t {
        std::size_t at = 0;
        while (at < cursor) {
            const auto next = ttytk::grapheme::next_break(text, at);
            if (next >= cursor) break;
            at = next;
        }
        return at;
    }
    static auto key_name(const char32_t cp) -> std::string {
        switch (cp) {
        case 57349: return "Delete";
        case 57350: return "Left";
        case 57351: return "Right";
        case 57352: return "Up";
        case 57353: return "Down";
        case 57356: return "Home";
        case 57357: return "End";
        case 8: case 127: return "Backspace";
        case 9: return "Tab";
        case 10: case 13: return "Enter";
        case 27: return "Escape";
        default:
            if (cp >= 1 && cp <= 26) return std::string("C-") + static_cast<char>('a' + cp - 1);
            return ttytk::utf8::encode(cp);
        }
    }
    void remember() {
        if (undo_.size() == 64) undo_.pop_front();
        undo_.emplace_back(text_, cursor_);
    }
    void insert(const std::string_view text) {
        if (text.empty()) return;
        remember();
        text_.insert(cursor_, text);
        cursor_ += text.size();
        // Joining across the insertion boundary can create a larger grapheme.
        if (cursor_ < text_.size())
            cursor_ = ttytk::grapheme::next_break(text_, previous(text_, cursor_));
    }
    void erase(const std::size_t begin, const std::size_t end, const bool kill = false) {
        if (begin >= end) return;
        remember();
        if (kill) kill_.push(text_.substr(begin, end - begin));
        text_.erase(begin, end - begin);
        cursor_ = begin;
        if (cursor_ != 0 && cursor_ < text_.size()) {
            const auto next = ttytk::grapheme::next_break(text_, previous(text_, cursor_));
            cursor_ = std::max(cursor_, next);
        }
    }
    void move(const bool right, std::size_t count) {
        for (count = std::max(count, std::size_t{1}); count != 0; --count) {
            const auto next = right ? ttytk::grapheme::next_break(text_, cursor_) : previous(text_, cursor_);
            if (next == cursor_) break;
            cursor_ = next;
        }
    }
    void select_history(const std::size_t index) {
        history_pos_ = index;
        text_ = history_->entries()[index].text;
        cursor_ = text_.size();
    }
    void navigate_history(const bool older) {
        if (!history_ || history_->entries().empty()) return;
        const auto size = history_->entries().size();
        const auto index = std::min(history_pos_.value_or(size), size);
        if (older) {
            if (!history_pos_) draft_ = text_;
            if (index > 0) select_history(index - 1);
        } else if (index + 1 < size) select_history(index + 1);
        else if (history_pos_) {
            history_pos_.reset();
            text_ = draft_;
            cursor_ = text_.size();
        }
    }
    void search_history(const bool repeat) {
        if (!history_) return;
        const auto size = history_->entries().size();
        auto index = repeat ? std::min(history_pos_.value_or(size), size) : size;
        while (index != 0) {
            if (history_->entries()[--index].text.find(search_) != std::string::npos) {
                select_history(index);
                return;
            }
        }
        if (!repeat) { text_ = draft_; cursor_ = text_.size(); }
    }
    auto run(const std::string_view action) -> bool {
        if (action == "home") cursor_ = 0;
        else if (action == "end") cursor_ = text_.size();
        else if (action == "left") move(false, 1);
        else if (action == "right") move(true, 1);
        else if (action == "backspace") erase(previous(text_, cursor_), cursor_);
        else if (action == "delete") erase(cursor_, ttytk::grapheme::next_break(text_, cursor_));
        else if (action == "kill-end") erase(cursor_, text_.size(), true);
        else if (action == "kill-start") erase(0, cursor_, true);
        else if (action == "kill-word") {
            auto start = cursor_;
            while (start && (text_[start - 1] == ' ' || text_[start - 1] == '\t')) start = previous(text_, start);
            while (start && text_[start - 1] != ' ' && text_[start - 1] != '\t') start = previous(text_, start);
            erase(start, cursor_, true);
        } else if (action == "yank") insert(kill_.yank());
        else if (action == "undo") (void)undo();
        else if (action == "previous-history") navigate_history(true);
        else if (action == "next-history") navigate_history(false);
        else if (action == "complete") {
            const auto prefix = completion::Engine::common_prefix(complete());
            const auto typed = std::string_view(text_).substr(0, cursor_);
            if (prefix.starts_with(typed)) insert(std::string_view(prefix).substr(cursor_));
        } else return false;
        return true;
    }
    [[nodiscard]] auto needs_continuation() const -> bool {
        std::string brackets;
        char quote = 0;
        bool escaped = false;
        for (const char ch : text_) {
            if (escaped) { escaped = false; continue; }
            if (ch == '\\' && quote != '\'') { escaped = true; continue; }
            if (quote) { if (ch == quote) quote = 0; continue; }
            if (ch == '\'' || ch == '"') quote = ch;
            else if (ch == '(' || ch == '[' || ch == '{') brackets += ch;
            else if (ch == ')' || ch == ']' || ch == '}') {
                const auto pair = ch == ')' ? '(' : ch == ']' ? '[' : '{';
                if (brackets.empty() || brackets.back() != pair) return false;
                brackets.pop_back();
            }
        }
        return escaped || quote != 0 || !brackets.empty();
    }

    Prompt prompt_;
    std::string text_;
    std::size_t cursor_{};
    bool accepted_{};
    bool multiline_{true};
    bool searching_{};
    Mode mode_{Mode::emacs};
    std::size_t count_{};
    Keymap keymap_;
    KillRing kill_;
    std::deque<std::pair<std::string, std::size_t>> undo_;
    hist::History* history_{};
    std::optional<std::size_t> history_pos_;
    std::string draft_;
    std::string search_;
    std::function<std::vector<completion::Candidate>(std::string_view)> completer_{};
};

/// Return the accepted line after the caller has fed terminal events.
[[nodiscard]] inline auto read_line(Editor& editor) -> std::optional<std::string> {
    return editor.read();
}

}  // namespace ttyutils::rl
