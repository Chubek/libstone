#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <string>

namespace ttytk::bufutil {
namespace detail {
inline void append_style(std::string& out, const Cell& cell) {
    out += "\x1b[0";
    // Cell attributes use bit (SGR parameter - 1).
    for (unsigned attr = 1; attr <= 9; ++attr) {
        if ((cell.attributes & (1U << (attr - 1))) != 0)
            out += ';' + std::to_string(attr);
    }
    const auto append_rgb = [&out](const color::Rgb rgb) {
        out += std::to_string(rgb.red) + ';' + std::to_string(rgb.green) + ';' +
               std::to_string(rgb.blue);
    };
    out += ";38;2;";
    append_rgb(cell.foreground);
    out += ";48;2;";
    append_rgb(cell.background);
    out += 'm';
}
}  // namespace detail

[[nodiscard]] inline auto dump_text(const CellBuffer& buffer) -> std::string {
    std::string out{}; for(std::size_t row=0;row<buffer.rows();++row) { for(std::size_t col=0;col<buffer.columns();++col) if(!buffer.at(row,col).continuation) out+=buffer.at(row,col).grapheme; if(row+1<buffer.rows())out+='\n'; } return out;
}
/** Dumps text and style, restoring the terminal's default style afterward. */
[[nodiscard]] inline auto dump_ansi(const CellBuffer& buffer) -> std::string {
    std::string out;
    const Cell* style = nullptr;
    for (std::size_t row = 0; row < buffer.rows(); ++row) {
        if (row != 0) out += '\n';
        for (std::size_t col = 0; col < buffer.columns(); ++col) {
            const auto& cell = buffer.at(row, col);
            if (cell.continuation) continue;
            if (style == nullptr || style->attributes != cell.attributes ||
                style->foreground != cell.foreground || style->background != cell.background) {
                detail::append_style(out, cell);
                style = &cell;
            }
            out += cell.grapheme;
        }
    }
    if (style != nullptr) out += esc::sgr(esc::Reset);
    return out;
}

/** Compares dimensions, glyphs, styles, and wide-cell continuations. */
[[nodiscard]] inline auto compare(const CellBuffer& left, const CellBuffer& right) -> bool {
    if (left.rows() != right.rows() || left.columns() != right.columns()) return false;
    for (std::size_t row = 0; row < left.rows(); ++row)
        for (std::size_t col = 0; col < left.columns(); ++col)
            if (left.at(row, col) != right.at(row, col)) return false;
    return true;
}

/** Returns an ANSI update for changed cells, or a clear and redraw on resize. */
[[nodiscard]] inline auto diff(const CellBuffer& left, const CellBuffer& right) -> std::string {
    const bool resized = left.rows() != right.rows() || left.columns() != right.columns();
    std::string out = resized ? esc::clear() : std::string{};
    const auto changed = [&](const std::size_t row, const std::size_t col) {
        return resized || left.at(row, col) != right.at(row, col);
    };
    bool wrote = false;
    for (std::size_t row = 0; row < right.rows(); ++row) {
        for (std::size_t col = 0; col < right.columns(); ++col) {
            const auto& cell = right.at(row, col);
            if (cell.continuation) continue;
            const bool changed_tail = col + 1 < right.columns() &&
                right.at(row, col + 1).continuation && changed(row, col + 1);
            if (!changed(row, col) && !changed_tail) continue;
            out += esc::cursor_to(static_cast<unsigned>(row + 1), static_cast<unsigned>(col + 1));
            detail::append_style(out, cell);
            out += cell.grapheme.empty() ? " " : cell.grapheme;
            wrote = true;
        }
    }
    if (wrote) out += esc::sgr(esc::Reset);
    return out;
}
}  // namespace ttytk::bufutil
