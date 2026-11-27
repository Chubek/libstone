#pragma once

#include <TTyUtils/TTyUtils-Search.hpp>
#include <TTyUtils/TTyUtils-SyntaxHighlight.hpp>
#include <TTyUtils/TTyUtils-TextCursor.hpp>
#include <TTyUtils/TTyUtils-UndoHistory.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>
#include <TTyUtils/TTyUtils-WordWrap.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ttyutils::ui {

struct EditorOptions {
    bool wrap{};
    bool center_on_jump{};
    std::size_t scrolloff{2};
};

class Gutter {
public:
    void show_line_numbers(const bool value) noexcept { line_numbers_ = value; }
    [[nodiscard]] auto line_numbers() const noexcept -> bool { return line_numbers_; }

private:
    bool line_numbers_{true};
};

class TextArea : public Widget {
public:
    TextArea() : history_(buffer_, cursors_) { cursors_.add({0, 0}); }

    [[nodiscard]] auto open(const std::string& path) -> bool {
        auto loaded = textbuf::load_file(path);
        if (!loaded) return false;
        buffer_ = std::move(loaded).value();
        reset_view();
        return true;
    }
    void set_text(std::string text) {
        buffer_.replace_bytes(0, buffer_.size(), text);
        reset_view();
    }
    [[nodiscard]] auto text() const noexcept -> std::string_view { return buffer_.text(); }
    [[nodiscard]] auto options() noexcept -> EditorOptions& { return options_; }
    [[nodiscard]] auto gutter() noexcept -> Gutter& { return gutter_; }
    [[nodiscard]] auto cursor() noexcept -> textcur::CursorSet& { return cursors_; }
    [[nodiscard]] auto first_row() const noexcept -> std::size_t { return first_; }
    [[nodiscard]] auto matches(const search::Query& query) const -> std::vector<search::Match> {
        return search::find_all(buffer_.snapshot(), query);
    }
    void jump(const textcur::Position position) {
        const auto index = buffer_.line_index();
        const auto clamped = index.position(index.offset(position, text()), text());
        cursors_.restore({textcur::Cursor{clamped}});
        invalidate(rect());
        center_ = options_.center_on_jump;
    }

    auto handle(const ttytk::input::Event& event) -> bool override {
        if (const auto* paste = std::get_if<ttytk::input::Paste>(&event)) {
            std::string cleaned;
            for (const auto cp : ttytk::utf8::decode(paste->text))
                if (scalar_text(cp) || cp == U'\n' || cp == U'\t' || cp == U'\r') cleaned += ttytk::utf8::encode(cp);
            if (!cleaned.empty()) insert(cleaned, false);
            return true;
        }
        const auto* key = std::get_if<ttytk::input::Key>(&event);
        if (!key || key->event_type == 3) return false;
        const bool extend = (key->modifiers & 1) != 0;
        const bool ctrl = (key->modifiers & 4) != 0;
        const auto cp = key->codepoint;
        if (cp == 26 || (ctrl && (cp == U'z' || cp == U'Z'))) {
            const auto changed = extend ? history_.redo_branch() : history_.undo();
            if (changed) invalidate(rect());
            return true;
        }
        switch (cp) {
        case 57350: move(ctrl ? textcur::Motion::word_left : textcur::Motion::left, extend); return true;
        case 57351: move(ctrl ? textcur::Motion::word_right : textcur::Motion::right, extend); return true;
        case 57352: move(textcur::Motion::up, extend); return true;
        case 57353: move(textcur::Motion::down, extend); return true;
        case 127: case 8: erase(true); return true;
        case 57349: erase(false); return true;
        default: break;
        }
        if (key->modifiers & 0x3eU) return false;
        if (cp == U'\n' || cp == U'\r') {
            insert(buffer_.eol() == textbuf::Buffer::Eol::crlf ? "\r\n" : "\n", false);
            return true;
        }
        if (!key->text.empty()) {
            std::string value;
            for (const auto scalar : key->text)
                if (scalar_text(scalar)) value += ttytk::utf8::encode(scalar);
            if (!value.empty()) insert(value, true);
            return true;
        }
        if ((cp >= 57344 && cp <= 57454) || !scalar_text(cp)) return false;
        insert(ttytk::utf8::encode(cp), true);
        return true;
    }

    void paint(Canvas& canvas) override {
        const auto area = rect();
        if (area.empty()) return;
        const auto index = buffer_.line_index();
        const auto count = index.lines();
        const auto digits = std::to_string(count).size();
        const int margin = gutter_.line_numbers() ? std::min(area.width, static_cast<int>(digits) + 1) : 0;
        const auto width = area.width - margin;
        const auto rows = static_cast<std::size_t>(area.height);
        // This viewport must wrap even a single token wider than the pane.
        const auto map = options_.wrap && width > 0 ?
            wrap::soft_wrap(buffer_.snapshot(), static_cast<std::size_t>(width), wrap::BreakRules{true, false}) :
            wrap::WrapMap{};
        const bool wrapped = !map.rows().empty();
        const auto total = wrapped ? map.rows().size() : count;
        const auto at = cursors_.cursors().empty() ? textcur::Position{} : cursors_.cursors().front().position();
        const auto line = std::min(at.line, count - 1);
        const auto active = wrapped ? map.buffer_to_visual({line, at.col}).row : line;
        const auto pad = std::min(options_.scrolloff, (rows - 1) / 2);
        if (center_) { first_ = active - std::min(active, rows / 2); center_ = false; }
        else if (active < first_ || active - first_ < pad) first_ = active - std::min(active, pad);
        else if (active - first_ >= rows - pad) first_ = active - (rows - pad - 1);
        first_ = std::min(first_, total - std::min(total, rows));
        for (auto i = first_; i < total && i - first_ < rows; ++i) {
            const auto row = wrapped ? map.rows()[i] : wrap::WrapMap::Row{{i, 0}, {i, index.columns(i)}};
            const auto y = area.y + static_cast<int>(i - first_);
            if (margin && row.start.col == 0) {
                const auto number = std::to_string(row.start.line + 1);
                canvas.text(area.x, y, std::string(digits - number.size(), ' ') + number, style());
            }
            if (width > 0) {
                const auto start = index.offset(row.start, text());
                const auto end = index.offset(row.end, text());
                canvas.text(area.x + margin, y, fit_text(text().substr(start, end - start), width), style());
            }
        }
    }

private:
    static auto scalar_text(const char32_t cp) noexcept -> bool {
        return cp >= 0x20 && (cp < 0x7f || cp >= 0xa0) && !(cp >= 0xd800 && cp <= 0xdfff) && cp <= 0x10ffff;
    }
    void reset_view() {
        cursors_.restore({textcur::Cursor{}});
        history_ = undo::History{buffer_, cursors_};
        first_ = 0;
        center_ = false;
        invalidate(rect());
    }
    void move(const textcur::Motion motion, const bool extend) {
        cursors_.move(motion, extend);
        invalidate(rect());
    }
    void insert(const std::string_view value, const bool typing) {
        auto transaction = typing ? history_.typing_transaction() : history_.transaction();
        cursors_.insert_each(value);
        transaction.commit();
        invalidate(rect());
    }
    void erase(const bool backward) {
        auto transaction = history_.transaction();
        auto cursors = cursors_.cursors();
        for (auto& cursor : cursors) {
            const auto selected = cursor.selection();
            if (selected.anchor == selected.active)
                cursor.move(buffer_, backward ? textcur::Motion::left : textcur::Motion::right, true);
        }
        cursors_.restore(std::move(cursors));
        cursors_.insert_each({});
        transaction.commit();
        invalidate(rect());
    }

    textbuf::Buffer buffer_;
    textcur::CursorSet cursors_{buffer_};
    undo::History history_;
    EditorOptions options_{};
    Gutter gutter_{};
    std::size_t first_{};
    bool center_{};
};

}  // namespace ttyutils::ui
