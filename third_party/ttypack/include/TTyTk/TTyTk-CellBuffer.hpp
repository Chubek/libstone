#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CharWidth.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk {

/** @brief Cell rendition flags, stored at bit (SGR parameter - 1). */
enum class CellAttribute : std::uint32_t {
    Bold = 1U << 0, Faint = 1U << 1, Italic = 1U << 2, Underline = 1U << 3,
    Blink = 1U << 4, RapidBlink = 1U << 5, Inverse = 1U << 6,
    Conceal = 1U << 7, Strike = 1U << 8,
};

/** @brief A printable grapheme and its style, or a wide glyph's continuation. */
struct Cell {
    std::string grapheme{" "};
    std::uint32_t attributes{};
    color::Rgb foreground{229, 229, 229};
    color::Rgb background{0, 0, 0};
    bool continuation{};

    friend auto operator==(const Cell&, const Cell&) -> bool = default;
};

/** @brief Zero-based grid of styled terminal cells. */
class CellBuffer {
public:
    constexpr CellBuffer() = default;
    CellBuffer(const std::size_t rows, const std::size_t columns) { resize(rows, columns); }

    [[nodiscard]] inline auto rows() const noexcept -> std::size_t { return rows_; }
    [[nodiscard]] inline auto columns() const noexcept -> std::size_t { return columns_; }

    /** @brief Access a cell; use put() to change a glyph and its footprint. */
    [[nodiscard]] inline auto at(const std::size_t row, const std::size_t column) -> Cell& {
        return cells_[index(row, column)];
    }

    [[nodiscard]] inline auto at(const std::size_t row, const std::size_t column) const -> const Cell& {
        return cells_[index(row, column)];
    }

    /** @brief Write a glyph, clearing any wide glyphs overlapping its footprint. */
    inline void put(const std::size_t row, const std::size_t column, Cell cell) {
        if (row >= rows_ || column >= columns_) return;
        const auto width = glyph_width(cell.grapheme);
        const auto spans = width == 2 && column + 1 < columns_;
        if (width == 0 || (width == 2 && !spans)) cell.grapheme = " ";
        cell.continuation = false;
        clear_glyph(row, column);
        if (spans) clear_glyph(row, column + 1);
        at(row, column) = std::move(cell);
        if (spans) {
            at(row, column + 1) = Cell{"", at(row, column).attributes, at(row, column).foreground,
                                        at(row, column).background, true};
        }
    }

    /** @brief Preserve the intersecting region, dropping a wide glyph cut by the edge. */
    inline void resize(const std::size_t rows, const std::size_t columns) {
        std::vector<Cell> resized;
        if (columns != 0 && rows > resized.max_size() / columns)
            throw std::length_error("CellBuffer dimensions exceed capacity");
        resized.resize(rows * columns);
        const auto copy_rows = std::min(rows_, rows);
        const auto copy_columns = std::min(columns_, columns);
        for (std::size_t row = 0; copy_columns != 0 && row < copy_rows; ++row) {
            for (std::size_t column = 0; column < copy_columns; ++column) {
                if (column + 1 == columns && columns < columns_ && at(row, columns).continuation)
                    continue;
                resized[row * columns + column] = at(row, column);
            }
        }
        rows_ = rows;
        columns_ = columns;
        cells_ = std::move(resized);
    }

    /** @brief Shift rows in [top, bottom); positive amounts scroll upward. */
    inline void scroll(const std::size_t top, const std::size_t bottom, const int amount) {
        if (top >= bottom || bottom > rows_ || amount == 0 || columns_ == 0) return;
        const auto height = bottom - top;
        const auto magnitude = amount < 0 ? 0U - static_cast<unsigned>(amount)
                                          : static_cast<unsigned>(amount);
        const auto shift = std::min(height, static_cast<std::size_t>(magnitude));
        if (amount > 0) {
            for (std::size_t row = top; row < bottom - shift; ++row)
                for (std::size_t column = 0; column < columns_; ++column)
                    at(row, column) = at(row + shift, column);
            for (std::size_t row = bottom - shift; row < bottom; ++row)
                for (std::size_t column = 0; column < columns_; ++column) at(row, column) = Cell{};
        } else {
            for (std::size_t row = bottom; row-- > top + shift;)
                for (std::size_t column = 0; column < columns_; ++column)
                    at(row, column) = at(row - shift, column);
            for (std::size_t row = top; row < top + shift; ++row)
                for (std::size_t column = 0; column < columns_; ++column) at(row, column) = Cell{};
        }
    }

    inline void clear() { std::fill(cells_.begin(), cells_.end(), Cell{}); }

private:
    [[nodiscard]] auto index(std::size_t row, std::size_t column) const -> std::size_t {
        if (row >= rows_ || column >= columns_) throw std::out_of_range("CellBuffer coordinate");
        return row * columns_ + column;
    }

    [[nodiscard]] static auto glyph_width(std::string_view text) -> int {
        // put() accepts one grapheme. Refuse strings that cannot fit a single cell glyph.
        if (grapheme::next_break(text) != text.size()) return 0;
        return charwidth::of_grapheme(text);
    }

    void clear_glyph(std::size_t row, std::size_t column) {
        if (at(row, column).continuation && column > 0) at(row, column - 1) = Cell{};
        if (column + 1 < columns_ && at(row, column + 1).continuation)
            at(row, column + 1) = Cell{};
        at(row, column) = Cell{};
    }

    std::size_t rows_{};
    std::size_t columns_{};
    std::vector<Cell> cells_{};
};

// NOTE(agent): The manifests refer to a TTyTk::Screen header that does not
// exist. Screen is the minimal owning façade over the existing cell buffer.
class Screen {
public:
    Screen() = default;
    Screen(const std::size_t rows, const std::size_t columns) : buffer_(rows, columns) {}
    [[nodiscard]] auto rows() const noexcept -> std::size_t { return buffer_.rows(); }
    [[nodiscard]] auto columns() const noexcept -> std::size_t { return buffer_.columns(); }
    void resize(const std::size_t rows, const std::size_t columns) { buffer_.resize(rows, columns); }
    void clear() { buffer_.clear(); }
    [[nodiscard]] auto cells() noexcept -> CellBuffer& { return buffer_; }
    [[nodiscard]] auto cells() const noexcept -> const CellBuffer& { return buffer_; }

private:
    CellBuffer buffer_;
};

}  // namespace ttytk
