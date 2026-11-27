#pragma once

#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyUtils/TTyUtils-TextBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::textcur {

using Position = textbuf::Buffer::Position;

enum class Motion {
    left, right, up, down, word_left, word_right, paragraph_up, paragraph_down,
    subword_left, subword_right, matching_bracket
};

namespace detail {
[[nodiscard]] constexpr auto less(Position left, Position right) noexcept -> bool {
    return left.line < right.line || (left.line == right.line && left.col < right.col);
}

[[nodiscard]] constexpr auto word(const char32_t c) noexcept -> bool {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
           (c >= U'0' && c <= U'9') || c == U'_' ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c != 0xFEFF &&
            !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) &&
            !(c >= 0x1F000 && c <= 0x1FAFF));
}
}  // namespace detail

struct Selection {
    Position anchor{};
    Position active{};

    [[nodiscard]] auto begin() const noexcept -> Position { return std::min(anchor, active, detail::less); }
    [[nodiscard]] auto end() const noexcept -> Position { return std::max(anchor, active, detail::less); }
    [[nodiscard]] auto range() const noexcept -> textbuf::LineIndex::Range { return {begin(), end()}; }
};

/** Test a word transition at a grapheme boundary. */
[[nodiscard]] inline auto word_boundary(const std::string_view text, const std::size_t byte) -> bool {
    if (byte == 0 || byte >= text.size()) return true;
    for (std::size_t at = 0; at < byte;) {
        const auto next = ttytk::grapheme::next_break(text, at);
        if (next == byte)
            return detail::word(ttytk::detail::decode_utf8_scalar(text, at).value) !=
                   detail::word(ttytk::detail::decode_utf8_scalar(text, next).value);
        at = next;
    }
    return false;
}

class Cursor {
public:
    Cursor() = default;
    explicit Cursor(const Position position) : position_(position), anchor_(position), sticky_column_(position.col) {}
    [[nodiscard]] static auto from_selection(const Selection selection) -> Cursor {
        Cursor cursor{selection.active};
        cursor.anchor_ = selection.anchor;
        return cursor;
    }

    [[nodiscard]] auto position() const noexcept -> Position { return position_; }
    [[nodiscard]] auto selection() const noexcept -> Selection { return {anchor_, position_}; }
    void set_position(const Position position, const bool keep_anchor = false) noexcept {
        position_ = position;
        if (!keep_anchor) anchor_ = position;
        sticky_column_ = position.col;
    }

    void move(const textbuf::Buffer& buffer, const Motion motion, const bool extend = false) {
        const auto index = buffer.line_index();
        const auto lines = index.lines();
        position_ = index.position(index.offset(position_, buffer.text()), buffer.text());
        const auto line_length = [&index](const std::size_t line) { return index.columns(line); };
        const auto blank = [&buffer](std::size_t line) {
            return buffer.line(line).find_first_not_of(" \t") == std::string_view::npos;
        };
        switch (motion) {
        case Motion::left:
            if (position_.col > 0) --position_.col;
            else if (position_.line > 0) { --position_.line; position_.col = line_length(position_.line); }
            break;
        case Motion::right:
            if (position_.col < line_length(position_.line)) ++position_.col;
            else if (position_.line + 1 < lines) { ++position_.line; position_.col = 0; }
            break;
        case Motion::up:
            if (position_.line > 0) { --position_.line; position_.col = std::min(sticky_column_, line_length(position_.line)); }
            break;
        case Motion::down:
            if (position_.line + 1 < lines) { ++position_.line; position_.col = std::min(sticky_column_, line_length(position_.line)); }
            break;
        case Motion::word_left: move_word(buffer, false, false); break;
        case Motion::word_right: move_word(buffer, true, false); break;
        case Motion::subword_left: move_word(buffer, false, true); break;
        case Motion::subword_right: move_word(buffer, true, true); break;
        case Motion::matching_bracket: move_bracket(buffer); break;
        case Motion::paragraph_up:
            if (position_.line > 0 && position_.col == 0 &&
                (blank(position_.line) || blank(position_.line - 1))) --position_.line;
            while (position_.line > 0 && blank(position_.line)) --position_.line;
            while (position_.line > 0 && !blank(position_.line - 1)) --position_.line;
            position_.col = 0;
            break;
        case Motion::paragraph_down:
            while (position_.line + 1 < lines && !blank(position_.line)) ++position_.line;
            while (position_.line + 1 < lines && blank(position_.line)) ++position_.line;
            position_.col = 0;
            break;
        }
        if (!extend) anchor_ = position_;
        if (motion != Motion::up && motion != Motion::down) sticky_column_ = position_.col;
    }

private:
    void move_word(const textbuf::Buffer& buffer, const bool forward, const bool subword) {
        const auto index = buffer.line_index();
        const auto text = buffer.text();
        auto offset = index.offset(position_, text);
        const auto bom = index.offset({}, text);
        const auto next = [&](std::size_t at) { return ttytk::grapheme::next_break(text, at); };
        const auto prev = [&](std::size_t at) {
            auto pos = index.position(at - 1, text);
            return index.offset(pos, text);
        };
        const auto cp = [&](std::size_t at) {
            return at < text.size() ? ttytk::detail::decode_utf8_scalar(text, at).value : U'\0';
        };
        const auto is_word = [&](std::size_t at) { return detail::word(cp(at)) && (!subword || cp(at) != U'_'); };
        const auto upper = [](char32_t c) { return c >= U'A' && c <= U'Z'; };
        const auto lower = [](char32_t c) { return c >= U'a' && c <= U'z'; };
        const auto digit = [](char32_t c) { return c >= U'0' && c <= U'9'; };
        const auto split = [&](std::size_t left, std::size_t right) {
            if (!subword) return false;
            const auto a = cp(left);
            const auto b = cp(right);
            return digit(a) != digit(b) || (lower(a) && upper(b)) ||
                   (upper(a) && upper(b) && lower(cp(next(right))));
        };
        if (forward) {
            if (!is_word(offset)) {
                while (offset < text.size() && !is_word(offset)) offset = next(offset);
            } else {
                do {
                    const auto old = offset;
                    offset = next(offset);
                    if (split(old, offset)) break;
                } while (is_word(offset));
            }
        } else if (offset > bom) {
            do { offset = prev(offset); } while (offset > bom && !is_word(offset));
            while (offset > bom) {
                const auto prior = prev(offset);
                if (!is_word(prior) || split(prior, offset)) break;
                offset = prior;
            }
        }
        position_ = index.position(offset, text);
    }

    void move_bracket(const textbuf::Buffer& buffer) {
        constexpr std::string_view brackets{"()[]{}"};
        const auto index = buffer.line_index();
        const auto text = buffer.text();
        const auto start = index.offset(position_, text);
        if (start == text.size()) return;
        const auto bracket = brackets.find(text[start]);
        if (bracket == std::string_view::npos) return;
        const auto match = brackets[bracket ^ 1U];
        auto at = start;
        std::size_t depth = 1;
        const auto forward = bracket % 2 == 0;
        while (forward ? at + 1 < text.size() : at > 0) {
            if (forward) ++at;
            else --at;
            if (text[at] == text[start]) ++depth;
            else if (text[at] == match && --depth == 0) {
                position_ = index.position(at, text);
                return;
            }
        }
    }

    Position position_{};
    Position anchor_{};
    std::size_t sticky_column_{};
};

/** Multi-cursor selections bound to a buffer that must outlive the set. */
class CursorSet {
public:
    explicit CursorSet(textbuf::Buffer& buffer) : buffer_(&buffer), state_(std::make_shared<State>()) { observe(); }
    CursorSet(const CursorSet& other) : CursorSet(*other.buffer_) { state_->cursors = other.cursors(); }
    CursorSet(CursorSet&&) noexcept = default;
    auto operator=(CursorSet&&) noexcept -> CursorSet& = default;
    auto operator=(const CursorSet& other) -> CursorSet& {
        if (this != &other) *this = CursorSet{other};
        return *this;
    }

    void add(const Position position) { state_->cursors.emplace_back(clamp(position)); merge(*state_); }
    void add_selection(const Selection selection) {
        state_->cursors.push_back(Cursor::from_selection({clamp(selection.anchor), clamp(selection.active)}));
        merge(*state_);
    }
    [[nodiscard]] auto cursors() const noexcept -> const std::vector<Cursor>& { return state_->cursors; }
    void restore(std::vector<Cursor> cursors) {
        for (auto& cursor : cursors) {
            const auto selected = cursor.selection();
            const Selection clamped{clamp(selected.anchor), clamp(selected.active)};
            if (selected.anchor != clamped.anchor || selected.active != clamped.active)
                cursor = Cursor::from_selection(clamped);
        }
        state_->cursors = std::move(cursors);
        merge(*state_);
    }

    void move(const Motion motion, const bool extend = false) {
        for (auto& cursor : state_->cursors) cursor.move(*buffer_, motion, extend);
        merge(*state_);
    }

    void insert_each(const std::string_view text) {
        const std::string owned{text};  // May alias a view of the buffer.
        struct Batch {
            bool& active;
            explicit Batch(bool& flag) : active(flag) { active = true; }
            ~Batch() { active = false; }
        } batch{state_->batch};
        for (auto it = state_->cursors.rbegin(); it != state_->cursors.rend(); ++it) {
            const auto selection = it->selection();
            const auto index = buffer_->line_index();
            const auto first = index.offset(selection.begin(), buffer_->text());
            const auto last = index.offset(selection.end(), buffer_->text());
            buffer_->replace_bytes(first, last, owned);
            it->set_position(after_byte(*buffer_, first + owned.size()));
        }
        merge(*state_);
    }

private:
    struct State {
        std::vector<Cursor> cursors;
        bool batch{};
    };

    [[nodiscard]] auto clamp(Position p) const -> Position {
        const auto index = buffer_->line_index();
        return index.position(index.offset(p, buffer_->text()), buffer_->text());
    }

    [[nodiscard]] static auto after_byte(const textbuf::Buffer& buffer, std::size_t byte) -> Position {
        const auto index = buffer.line_index();
        auto position = index.position(byte, buffer.text());
        if (byte > index.offset(position, buffer.text()) && position.col < index.columns(position.line))
            ++position.col;
        return position;
    }

    void observe() {
        subscription_ = buffer_->observe_edits([state = state_, buffer = buffer_](const textbuf::Buffer::Edit& edit) {
            const auto old = edit.before.line_index();
            const auto remap = [&](Position position) {
                const auto byte = old.offset(position, edit.before.text());
                if (byte < edit.offset) return buffer->line_index().position(byte, buffer->text());
                return after_byte(*buffer, byte <= edit.offset + edit.erased ? edit.offset + edit.inserted
                    : byte - edit.erased + edit.inserted);
            };
            for (auto& cursor : state->cursors) {
                const auto selected = cursor.selection();
                if (const auto anchor = remap(selected.anchor), active = remap(selected.active);
                    anchor != selected.anchor || active != selected.active)
                    cursor = Cursor::from_selection({anchor, active});
            }
            if (!state->batch) merge(*state);
        });
    }

    static void merge(State& state) {
        auto& cursors = state.cursors;
        std::sort(cursors.begin(), cursors.end(), [](const Cursor& left, const Cursor& right) {
            return detail::less(left.selection().begin(), right.selection().begin());
        });
        std::size_t count = 0;
        for (auto& cursor : cursors) {
            const auto next = cursor.selection();
            const auto prior_end = count == 0 ? Position{} : cursors[count - 1].selection().end();
            if (count == 0 || detail::less(prior_end, next.begin()) ||
                (prior_end == next.begin() &&
                 cursors[count - 1].selection().begin() != prior_end && next.begin() != next.end())) {
                cursors[count++] = cursor;
            } else {
                auto& prior = cursors[count - 1];
                const auto selected = prior.selection();
                const auto end = std::max(selected.end(), next.end(), detail::less);
                if (end != selected.end()) prior = Cursor::from_selection(detail::less(selected.active, selected.anchor)
                    ? Selection{end, selected.begin()} : Selection{selected.begin(), end});
            }
        }
        cursors.resize(count);
    }

    textbuf::Buffer* buffer_{};
    std::shared_ptr<State> state_;
    textbuf::Buffer::EditSubscription subscription_;
};

}  // namespace ttyutils::textcur
