#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk {

/** @brief An inclusive selection span on a single row. */
struct SelectionRect {
    std::size_t row{}, first{}, last{};
    friend auto operator==(const SelectionRect&, const SelectionRect&) -> bool = default;
};

/** @brief Drag selection with character, word, line, or rectangular granularity. */
class Selection {
public:
    enum class Mode { Character, Word, Line, Block };

    void start(std::size_t row, std::size_t column, Mode mode = Mode::Character) noexcept {
        anchor_ = cursor_ = {row, column};
        mode_ = mode;
        active_ = true;
    }

    void update(std::size_t row, std::size_t column) noexcept { cursor_ = {row, column}; }

    /** @brief Return raw geometry; an open right edge is represented by SIZE_MAX.
     * @see get_rects(const CellBuffer&) for clipped and word-snapped geometry.
     */
    [[nodiscard]] auto get_rects() const -> std::vector<SelectionRect> {
        return active_ ? spans(anchor_, cursor_, std::numeric_limits<std::size_t>::max())
                       : std::vector<SelectionRect>{};
    }

    /** @brief Clip geometry to the grid and include entire endpoint glyphs. */
    [[nodiscard]] auto get_rects(const CellBuffer& buffer) const -> std::vector<SelectionRect> {
        if (!active_ || buffer.rows() == 0 || buffer.columns() == 0) return {};
        auto first = std::min(anchor_, cursor_);
        auto last = std::max(anchor_, cursor_);
        if (first.first >= buffer.rows()) return {};
        const auto last_column = buffer.columns() - 1;
        first.second = std::min(first.second, last_column);
        if (last.first >= buffer.rows()) {
            last.first = buffer.rows() - 1;
            if (mode_ != Mode::Block) last.second = last_column;
        }
        last.second = std::min(last.second, last_column);

        auto rects = spans(first, last, last_column);
        for (auto& rect : rects) {
            if (buffer.at(rect.row, rect.first).continuation && rect.first != 0) --rect.first;
            if (mode_ == Mode::Word) {
                // Expand to the character class at each endpoint, not just to ASCII spaces.
                const auto first_class = word_class(buffer.at(rect.row, rect.first).grapheme);
                while (rect.first != 0) {
                    auto previous = rect.first - 1;
                    if (buffer.at(rect.row, previous).continuation && previous != 0) --previous;
                    if (word_class(buffer.at(rect.row, previous).grapheme) != first_class) break;
                    rect.first = previous;
                }
                auto lead = rect.last;
                if (buffer.at(rect.row, lead).continuation && lead != 0) --lead;
                const auto last_class = word_class(buffer.at(rect.row, lead).grapheme);
                while (rect.last < last_column) {
                    const auto& next = buffer.at(rect.row, rect.last + 1);
                    if (!next.continuation && word_class(next.grapheme) != last_class) break;
                    ++rect.last;
                }
            } else if (rect.last < last_column && buffer.at(rect.row, rect.last + 1).continuation) {
                ++rect.last;
            }
        }
        return rects;
    }

    /** @brief Extract graphemes, joining selected rows with newlines. */
    [[nodiscard]] auto to_text(const CellBuffer& buffer) const -> std::string {
        std::string out;
        bool first_row = true;
        for (const auto rect : get_rects(buffer)) {
            if (!first_row) out.push_back('\n');
            first_row = false;
            for (auto column = rect.first; column <= rect.last; ++column) {
                const auto& cell = buffer.at(rect.row, column);
                if (!cell.continuation) out += cell.grapheme;
            }
        }
        return out;
    }

private:
    using Position = std::pair<std::size_t, std::size_t>;

    [[nodiscard]] auto spans(Position first, Position last, std::size_t right) const
        -> std::vector<SelectionRect> {
        if (first > last) std::swap(first, last);
        std::vector<SelectionRect> rects;
        for (auto row = first.first;; ++row) {
            if (mode_ == Mode::Block) {
                rects.push_back({row, std::min(first.second, last.second),
                                      std::max(first.second, last.second)});
            } else if (mode_ == Mode::Line) {
                rects.push_back({row, 0, right});
            } else {
                rects.push_back({row, row == first.first ? first.second : 0,
                                      row == last.first ? last.second : right});
            }
            if (row == last.first) return rects;
        }
    }

    [[nodiscard]] static auto word_class(std::string_view glyph) noexcept -> unsigned {
        if (glyph.empty()) return 0;
        const auto byte = static_cast<unsigned char>(glyph.front());
        if (byte == ' ' || (byte >= '\t' && byte <= '\r')) return 0;
        if (byte >= 128 || (byte >= '0' && byte <= '9') ||
            (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || byte == '_')
            return 256;
        return byte;
    }

    Position anchor_{}, cursor_{};
    Mode mode_{Mode::Character};
    bool active_{};
};

} // namespace ttytk
