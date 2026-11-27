#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-TermMode.hpp>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttytk {

/** Headless terminal screen for replaying application output. */
class VirtualTerm {
public:
    explicit VirtualTerm(const std::size_t rows = 24, const std::size_t columns = 80)
        : screen_(rows, columns), rows_(rows), columns_(columns), bottom_(rows) {
        modes_.set(Mode::AutoWrap);
    }

    /** Apply terminal output bytes, retaining parser and UTF-8 state across chunks. */
    inline void input(const std::string_view bytes) {
        sync_geometry();
        // Bind to the current object, including after a copy or move mid-stream.
        parser_.on_print = [this](const std::string_view text) { put_text(text); };
        parser_.on_control = [this](const unsigned char control) { execute(control); };
        parser_.on_escape = [this](const std::string_view escape) { dispatch_escape(escape); };
        parser_.on_csi = [this](const std::string_view csi) { dispatch_csi(csi); };
        parser_.on_osc = parser_.on_dcs = [this](std::string_view) { flush_partial(); };
        const auto offset = output_.size();
        output_.append(bytes);
        parser_.feed(std::string_view{output_}.substr(offset));
    }

    /** Raw input transcript, not a response stream for device queries. */
    [[nodiscard]] inline auto output() const noexcept -> std::string_view { return output_; }
    [[nodiscard]] inline auto screen() noexcept -> CellBuffer& { return screen_; }
    [[nodiscard]] inline auto screen() const noexcept -> const CellBuffer& { return screen_; }

private:
    struct Position {
        std::size_t row{}, column{};
    };

    struct Cursor {
        Position position{};
        Cell rendition{};
        bool origin{};
    };

    [[nodiscard]] inline auto ready() const noexcept -> bool { return rows_ != 0 && columns_ != 0; }
    [[nodiscard]] static auto last(const std::size_t extent) noexcept -> std::size_t {
        return extent == 0 ? 0 : extent - 1;
    }

    inline void sync_geometry() {
        if (rows_ == screen_.rows() && columns_ == screen_.columns()) return;
        rows_ = screen_.rows();
        columns_ = screen_.columns();
        top_ = 0;
        bottom_ = rows_;
        move(cursor_.row, cursor_.column);
    }

    inline void move(const std::size_t row, const std::size_t column) {
        cursor_ = {std::min(row, last(rows_)), std::min(column, last(columns_))};
        wrap_pending_ = false;
        previous_.reset();
    }

    inline void save() { saved_ = Cursor{cursor_, rendition_, origin_}; }

    inline void restore() {
        if (!saved_) return;
        origin_ = saved_->origin;
        rendition_ = saved_->rendition;
        move(saved_->position.row, saved_->position.column);
    }

    inline void scroll(const std::size_t top, const std::size_t bottom,
                       const std::size_t count, const bool up) {
        if (top >= bottom) return;
        const auto shift = std::min({count, bottom - top, static_cast<std::size_t>(INT_MAX)});
        screen_.scroll(top, bottom, up ? static_cast<int>(shift) : -static_cast<int>(shift));
        for (std::size_t row = up ? bottom - shift : top; row < (up ? bottom : top + shift); ++row)
            erase_line(row, 0, columns_);
    }

    inline void index(const bool reverse = false) {
        if (!ready()) return;
        if (!reverse && cursor_.row == last(bottom_)) scroll(top_, bottom_, 1, true);
        else if (reverse && cursor_.row == top_) scroll(top_, bottom_, 1, false);
        else if (reverse && cursor_.row > 0) --cursor_.row;
        else if (!reverse && cursor_.row < last(rows_)) ++cursor_.row;
        move(cursor_.row, cursor_.column);
    }

    inline void execute(const unsigned char control) {
        flush_partial();
        if (!ready()) return;
        // VT100 C0 controls and their xterm C1 counterparts.
        switch (control) {
        case '\b': move(cursor_.row, cursor_.column == 0 ? 0 : cursor_.column - 1); break;
        case '\t': move(cursor_.row, cursor_.column + (8 - cursor_.column % 8)); break;
        case '\r': move(cursor_.row, 0); break;
        case '\n': case '\v': case '\f':
            if (newline_) move(cursor_.row, 0);
            index();
            break;
        case 0x84: index(); break;
        case 0x85: move(cursor_.row, 0); index(); break;
        case 0x8D: index(true); break;
        default: break;
        }
    }

    inline void dispatch_escape(const std::string_view sequence) {
        flush_partial();
        if (sequence == "D") index();
        else if (sequence == "E") { move(cursor_.row, 0); index(); }
        else if (sequence == "M") index(true);
        else if (sequence == "7") save();
        else if (sequence == "8") restore();
        else if (sequence == "c") {
            alternate(false);
            screen_.clear();
            saved_.reset();
            rendition_ = {};
            modes_ = {};
            modes_.set(Mode::AutoWrap);
            origin_ = newline_ = false;
            top_ = 0;
            bottom_ = rows_;
            move(0, 0);
        }
    }

    inline void erase_line(const std::size_t row, const std::size_t first, const std::size_t end) {
        for (auto column = first; column < end; ++column)
            screen_.put(row, column, rendition_);
    }

    inline void erase_display(const unsigned mode) {
        if (mode > 2) return;
        for (auto row = mode == 0 ? cursor_.row : 0; row < (mode == 1 ? cursor_.row + 1 : rows_); ++row) {
            const auto start = row == cursor_.row && mode == 0 ? cursor_.column : 0;
            const auto end = row == cursor_.row && mode == 1 ? cursor_.column + 1 : columns_;
            erase_line(row, start, end);
        }
    }

    inline void alternate(const bool enabled) {
        if (enabled && !primary_) {
            CellBuffer alternate_screen{rows_, columns_};
            primary_ = std::move(screen_);
            primary_cursor_ = Cursor{cursor_, rendition_, origin_};
            screen_ = std::move(alternate_screen);
            move(0, 0);
        } else if (!enabled && primary_) {
            primary_->resize(rows_, columns_);
            screen_ = std::move(*primary_);
            primary_.reset();
            rendition_ = primary_cursor_.rendition;
            origin_ = primary_cursor_.origin;
            move(primary_cursor_.position.row, primary_cursor_.position.column);
        }
    }

    inline void sgr(const std::vector<unsigned>& parameters) {
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            const auto parameter = parameters[index];
            if (parameter == 0) rendition_ = {};
            else if (parameter <= 9) rendition_.attributes |= 1U << (parameter - 1);
            else if (parameter == 22) rendition_.attributes &= ~3U;
            else if (parameter == 25) rendition_.attributes &= ~((1U << 4) | (1U << 5));
            else if (parameter == 23 || parameter == 24 || (parameter >= 27 && parameter <= 29))
                rendition_.attributes &= ~(1U << (parameter - 21));
            else if (parameter == 39) rendition_.foreground = Cell{}.foreground;
            else if (parameter == 49) rendition_.background = Cell{}.background;
            else if ((parameter >= 30 && parameter <= 37) || (parameter >= 40 && parameter <= 47) ||
                     (parameter >= 90 && parameter <= 97) || (parameter >= 100 && parameter <= 107)) {
                auto& target = (parameter / 10 == 3 || parameter / 10 == 9)
                                   ? rendition_.foreground : rendition_.background;
                const auto palette = parameter % 10 + (parameter >= 90 ? 8U : 0U);
                target = color::detail::palette_256(static_cast<std::uint8_t>(palette));
            } else if ((parameter == 38 || parameter == 48) && ++index < parameters.size()) {
                auto& target = parameter == 38 ? rendition_.foreground : rendition_.background;
                if (parameters[index] == 5) {
                    if (++index == parameters.size()) return;
                    if (parameters[index] <= 255)
                        target = color::detail::palette_256(static_cast<std::uint8_t>(parameters[index]));
                } else if (parameters[index] == 2) {
                    if (parameters.size() - index <= 3) return;
                    const auto red = parameters[++index];
                    const auto green = parameters[++index];
                    const auto blue = parameters[++index];
                    if (red <= 255 && green <= 255 && blue <= 255)
                        target = {static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green),
                                  static_cast<std::uint8_t>(blue)};
                }
            }
        }
    }

    inline void dispatch_csi(std::string_view sequence) {
        flush_partial();
        if (sequence.empty()) return;
        const auto final = sequence.back();
        sequence.remove_suffix(1);
        const bool dec = sequence.starts_with('?');
        if (dec) sequence.remove_prefix(1);
        std::vector<unsigned> parameters;
        for (;;) {
            const auto separator = sequence.find(';');
            const auto part = sequence.substr(0, separator);
            unsigned value{};
            if (!part.empty()) {
                const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) return;
            }
            parameters.push_back(value);
            if (separator == std::string_view::npos) break;
            sequence.remove_prefix(separator + 1);
        }
        if (final == 'h' || final == 'l') {
            const bool enabled = final == 'h';
            for (const auto parameter : parameters) {
                if (!dec && parameter == 20) newline_ = enabled; // ANSI LNM
                else if (dec && parameter == 7) {
                    if (enabled) modes_.set(Mode::AutoWrap);
                    else modes_.reset(Mode::AutoWrap);
                    wrap_pending_ = false;
                } else if (dec && parameter == 6) {
                    origin_ = enabled;
                    move(origin_ ? top_ : 0, 0);
                } else if (dec && parameter == 1049) alternate(enabled);
            }
            return;
        }
        if (dec || !ready()) return;
        const auto arg = [&](const std::size_t index, const std::size_t fallback = 1) {
            return index >= parameters.size() || parameters[index] == 0
                       ? fallback : static_cast<std::size_t>(parameters[index]);
        };
        const auto vertical_top = cursor_.row >= top_ ? top_ : 0;
        const auto vertical_last = cursor_.row < bottom_ ? last(bottom_) : last(rows_);
        const auto up = [&] { return cursor_.row - std::min(cursor_.row - vertical_top, arg(0)); };
        const auto down = [&] { return cursor_.row + std::min(vertical_last - cursor_.row, arg(0)); };
        const auto absolute_row = [&] {
            return origin_ ? top_ + std::min(arg(0) - 1, last(bottom_ - top_)) : arg(0) - 1;
        };
        // ECMA-48 cursor/erase/scroll controls and xterm's DECSTBM.
        switch (final) {
        case 'm': sgr(parameters); break;
        case 'A': move(up(), cursor_.column); break;
        case 'B': case 'e': move(down(), cursor_.column); break;
        case 'C': case 'a':
            move(cursor_.row, cursor_.column + std::min(last(columns_) - cursor_.column, arg(0)));
            break;
        case 'D': move(cursor_.row, cursor_.column - std::min(cursor_.column, arg(0))); break;
        case 'E': move(down(), 0); break;
        case 'F': move(up(), 0); break;
        case 'G': case '`': move(cursor_.row, arg(0) - 1); break;
        case 'd': move(absolute_row(), cursor_.column); break;
        case 'H': case 'f': move(absolute_row(), arg(1) - 1); break;
        case 'J': erase_display(parameters[0]); previous_.reset(); break;
        case 'K':
            if (parameters[0] <= 2)
                erase_line(cursor_.row, parameters[0] == 0 ? cursor_.column : 0,
                           parameters[0] == 1 ? cursor_.column + 1 : columns_);
            previous_.reset();
            break;
        case 'X':
            erase_line(cursor_.row, cursor_.column,
                       cursor_.column + std::min(columns_ - cursor_.column, arg(0)));
            previous_.reset();
            break;
        case 'L': case 'M':
            if (cursor_.row >= top_ && cursor_.row < bottom_) {
                scroll(cursor_.row, bottom_, arg(0), final == 'M');
                move(cursor_.row, 0);
            }
            break;
        case 'S': case 'T':
            scroll(top_, bottom_, arg(0), final == 'S');
            previous_.reset();
            break;
        case 'r':
            if (const auto top = arg(0) - 1, bottom = arg(1, rows_);
                bottom <= rows_ && top < last(bottom)) {
                top_ = top;
                bottom_ = bottom;
                move(origin_ ? top_ : 0, 0);
            }
            break;
        case 's': save(); break;
        case 'u': restore(); break;
        default: break;
        }
    }

    inline void flush_partial() {
        if (partial_.empty()) return;
        partial_.clear();
        put_scalar("\xEF\xBF\xBD");
    }

    inline void put_text(const std::string_view text) {
        for (const auto character : text) {
            const auto byte = static_cast<unsigned char>(character);
            if (!partial_.empty() && (byte & 0xC0U) != 0x80U) flush_partial();
            if (partial_.empty()) {
                expected_ = byte >= 0xC2 && byte <= 0xDF ? 2 :
                            byte >= 0xE0 && byte <= 0xEF ? 3 :
                            byte >= 0xF0 && byte <= 0xF4 ? 4 : 1;
            }
            partial_ += character;
            if (partial_.size() == expected_) {
                put_scalar(utf8::validate(partial_) ? std::string_view{partial_} : "\xEF\xBF\xBD");
                partial_.clear();
            }
        }
    }

    inline void advance(const std::size_t width) {
        const auto end = cursor_.column + width;
        wrap_pending_ = end >= columns_ && modes_.test(Mode::AutoWrap);
        cursor_.column = std::min(end, last(columns_));
    }

    inline void put_scalar(const std::string_view scalar) {
        if (!ready()) return;
        if (previous_) {
            auto cell = screen_.at(previous_->row, previous_->column);
            const auto boundary = cell.grapheme.size();
            cell.grapheme += scalar;
            if (!cell.continuation && boundary != 0 && grapheme::next_break(cell.grapheme) > boundary) {
                const auto width = static_cast<std::size_t>(std::clamp(charwidth::of_grapheme(cell.grapheme), 1, 2));
                if (width <= columns_ - previous_->column) {
                    cursor_ = *previous_;
                    screen_.put(cursor_.row, cursor_.column, std::move(cell));
                    advance(width);
                    return;
                }
            }
        }
        const auto cell_width = charwidth::of_string(scalar);
        if (cell_width <= 0) return;
        const auto width = static_cast<std::size_t>(cell_width);
        if (width > columns_) return;
        if (wrap_pending_ || width > columns_ - cursor_.column) {
            if (!modes_.test(Mode::AutoWrap)) return;
            index();
            move(cursor_.row, 0);
        }
        auto cell = rendition_;
        cell.grapheme = scalar;
        screen_.put(cursor_.row, cursor_.column, std::move(cell));
        previous_ = cursor_;
        advance(width);
    }

    Automaton parser_{};
    CellBuffer screen_{};
    TermMode modes_{};
    std::string output_{};
    std::string partial_{};
    std::size_t expected_{};
    std::size_t rows_{}, columns_{}, top_{}, bottom_{};
    Position cursor_{};
    std::optional<Position> previous_{};
    std::optional<Cursor> saved_{};
    std::optional<CellBuffer> primary_{};
    Cursor primary_cursor_{};
    Cell rendition_{};
    bool wrap_pending_{};
    bool origin_{};
    bool newline_{};
};

}  // namespace ttytk
