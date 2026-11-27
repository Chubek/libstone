#pragma once

#include <TTyTk/TTyTk-Automaton.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-CharWidth.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>
#include <TTyTk/TTyTk-Encoding.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyTk/TTyTk-PsuedoTerm.hpp>
#include <TTyTk/TTyTk-Winsize.hpp>
#include <TTyUtils/TTyUtils-EventLoop.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ttyutils::term {

/// Bounded history of primary screen rows that have scrolled off the top.
class Scrollback {
public:
    explicit Scrollback(const std::size_t limit = 10000) : limit_(limit) {}

    void push(std::string line) {
        if (limit_ == 0) return;
        if (lines_.size() == limit_) lines_.erase(lines_.begin());
        lines_.push_back(std::move(line));
    }
    [[nodiscard]] auto lines() const noexcept -> const std::vector<std::string>& { return lines_; }
    void clear() { lines_.clear(); }

private:
    std::size_t limit_;
    std::vector<std::string> lines_;
};

/// Streaming VT/xterm emulator for an embedded terminal.
class Emulator {
public:
    Emulator(const std::size_t rows = 24, const std::size_t columns = 80)
        : cells_(rows, columns), bottom_(rows) {}

    /// Clear session state but keep dimensions and callbacks.
    void reset() {
        parser_.reset();
        cells_.clear();
        primary_.reset();
        scrollback_.clear();
        utf8_.clear();
        top_ = 0;
        bottom_ = cells_.rows();
        saved_ = {};
        style_ = {};
        autowrap_ = true;
        mouse_reporting_ = 0;
        move_to(0, 0);
    }

    void resize(const std::size_t rows, const std::size_t columns) {
        cells_.resize(rows, columns);
        if (primary_) primary_->resize(rows, columns);
        top_ = 0;
        bottom_ = rows;
        move_to(row_, column_);
    }

    /// Accept arbitrarily chunked PTY output. The parser retains incomplete sequences.
    void feed(const std::string_view bytes) {
        // Bind for this call so copied/moved emulators never use another object's state.
        parser_.on_print = [this](std::string_view text) { print(text); };
        parser_.on_control = [this](unsigned char control) { break_text(); execute(control); };
        parser_.on_csi = [this](std::string_view sequence) { break_text(); handle_csi(sequence); };
        parser_.on_osc = [this](std::string_view sequence) { break_text(); handle_osc(sequence); };
        parser_.on_escape = [this](std::string_view sequence) { break_text(); handle_escape(sequence); };
        parser_.on_dcs = [this](std::string_view) { break_text(); };
        parser_.feed(bytes);
    }

    /// Legacy plain-text view; each row is UTF-8 (wide continuations are omitted).
    [[nodiscard]] auto screen() const -> const std::vector<std::string>& {
        text_.resize(cells_.rows());
        for (std::size_t row = 0; row < cells_.rows(); ++row) text_[row] = row_text(row);
        return text_;
    }
    [[nodiscard]] auto cells() const noexcept -> const ttytk::CellBuffer& { return cells_; }
    [[nodiscard]] auto scrollback() noexcept -> Scrollback& { return scrollback_; }
    void on_osc(std::function<void(std::string, std::string)> callback) { osc_callback_ = std::move(callback); }
    void on_title(std::function<void(std::string)> callback) { title_callback_ = std::move(callback); }
    void on_cwd(std::function<void(std::string)> callback) { cwd_callback_ = std::move(callback); }
    [[nodiscard]] auto mouse_reporting() const noexcept -> bool { return mouse_reporting_ != 0; }

private:
    auto row_text(const std::size_t row) const -> std::string {
        std::string result;
        for (std::size_t column = 0; column < cells_.columns(); ++column) {
            const auto& cell = cells_.at(row, column);
            if (!cell.continuation) result += cell.grapheme;
        }
        return result;
    }
    void move_to(const std::size_t row, const std::size_t column) {
        row_ = std::min(row, cells_.rows() == 0 ? 0 : cells_.rows() - 1);
        column_ = std::min(column, cells_.columns() == 0 ? 0 : cells_.columns() - 1);
        wrap_pending_ = false;
        previous_glyph_.reset();
    }
    void index() {
        if (cells_.rows() == 0) return;
        if (row_ >= top_ && row_ + 1 == bottom_) {
            if (!primary_ && top_ == 0 && cells_.columns() != 0) scrollback_.push(row_text(top_));
            cells_.scroll(top_, bottom_, 1);
            move_to(row_, column_);
        } else move_to(row_ + 1, column_);
    }
    void execute(const unsigned char control) {
        switch (control) {
        case '\r': move_to(row_, 0); break;
        case '\n':
        case '\v':
        case '\f': index(); break;
        case '\b': move_to(row_, column_ == 0 ? 0 : column_ - 1); break;
        case '\t': move_to(row_, column_ + (8 - column_ % 8)); break;
        default: break;
        }
    }

    void print(const std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            if (!utf8_.empty() && (byte & 0xC0U) != 0x80U) break_text();
            if (utf8_.empty()) {
                if (byte < 0x80U) {
                    put(std::string(1, static_cast<char>(byte)));
                    continue;
                }
                utf8_length_ = byte >= 0xC2U && byte <= 0xDFU ? 2 :
                               byte >= 0xE0U && byte <= 0xEFU ? 3 :
                               byte >= 0xF0U && byte <= 0xF4U ? 4 : 0;
                if (utf8_length_ == 0) { put("\xEF\xBF\xBD"); continue; }
            }
            utf8_ += static_cast<char>(byte);
            if (utf8_.size() == utf8_length_) {
                const auto scalar = ttytk::utf8::decode(utf8_);
                if (scalar.size() == 1) {
                    utf8_.clear();
                    put(ttytk::utf8::encode(scalar));
                }
                else break_text();
            }
        }
    }
    void break_text() {
        if (!utf8_.empty()) { utf8_.clear(); put("\xEF\xBF\xBD"); }
        previous_glyph_.reset();
    }
    void put(std::string glyph) {
        const auto columns = cells_.columns();
        if (cells_.rows() == 0 || columns == 0) return;
        if (previous_glyph_) {
            const auto [row, column] = *previous_glyph_;
            auto cell = cells_.at(row, column);
            cell.grapheme += glyph;
            if (ttytk::grapheme::next_break(cell.grapheme) == cell.grapheme.size()) {
                const auto width = std::clamp(ttytk::charwidth::of_grapheme(cell.grapheme), 1, 2);
                if (column + static_cast<unsigned>(width) <= columns) {
                    cells_.put(row, column, std::move(cell));
                    advance(column, static_cast<unsigned>(width));
                }
                return;
            }
        }
        const auto width = static_cast<unsigned>(std::clamp(ttytk::charwidth::of_grapheme(glyph), 0, 2));
        if (width == 0 || width > columns) return;
        if (wrap_pending_ || width > columns - column_) {
            if (!autowrap_) return;
            move_to(row_, 0);
            index();
        }
        auto cell = style_;
        cell.grapheme = std::move(glyph);
        cells_.put(row_, column_, std::move(cell));
        previous_glyph_ = {row_, column_};
        advance(column_, width);
    }
    void advance(const std::size_t column, const unsigned width) {
        const auto end = column + width;
        wrap_pending_ = autowrap_ && end >= cells_.columns();
        column_ = std::min(end, cells_.columns() - 1);
    }

    static auto parameters(const std::string_view bytes) -> std::optional<std::vector<unsigned>> {
        std::vector<unsigned> values;
        auto rest = bytes;
        for (;;) {
            const auto end = rest.find(';');
            const auto field = rest.substr(0, end);
            unsigned value = 0;
            if (!field.empty()) {
                const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) return std::nullopt;
            }
            values.push_back(value);
            if (end == std::string_view::npos) return values;
            rest.remove_prefix(end + 1);
        }
    }
    static auto step(const std::size_t value, const std::size_t amount) -> std::size_t {
        return value > amount ? value - amount : 0;
    }
    static auto add(const std::size_t value, const std::size_t amount) -> std::size_t {
        return value + std::min(amount, std::numeric_limits<std::size_t>::max() - value);
    }
    void erase_line(const std::size_t row, const std::size_t begin, const std::size_t end) {
        for (auto column = begin; column < end; ++column) cells_.put(row, column, style_);
    }
    void erase(const char final, const unsigned mode) {
        if (mode == 3 && final == 'J') { scrollback_.clear(); return; }
        if (mode > 2 || cells_.rows() == 0 || cells_.columns() == 0) return;
        erase_line(row_, mode == 0 ? column_ : 0, mode == 1 ? column_ + 1 : cells_.columns());
        if (final == 'J') {
            if (mode != 0) for (std::size_t row = 0; row < row_; ++row)
                erase_line(row, 0, cells_.columns());
            if (mode != 1) for (auto row = row_ + 1; row < cells_.rows(); ++row)
                erase_line(row, 0, cells_.columns());
        }
        wrap_pending_ = false;
    }
    void rendition(const std::vector<unsigned>& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto code = values[i];
            using ttytk::color::from_256;
            if (code == 0) style_ = {};
            else if (code <= 9) style_.attributes |= 1U << (code - 1);
            else if (code == 22) style_.attributes &= ~3U;
            else if (code >= 23 && code <= 29) {
                style_.attributes &= ~(1U << (code - 21));
                if (code == 25) style_.attributes &= ~(1U << 5);
            } else if (code >= 30 && code <= 37) style_.foreground = from_256(code - 30);
            else if (code == 39) style_.foreground = ttytk::Cell{}.foreground;
            else if (code >= 40 && code <= 47) style_.background = from_256(code - 40);
            else if (code == 49) style_.background = ttytk::Cell{}.background;
            else if (code >= 90 && code <= 97) style_.foreground = from_256(code - 82);
            else if (code >= 100 && code <= 107) style_.background = from_256(code - 92);
            else if ((code == 38 || code == 48) && i + 1 < values.size()) {
                auto& color = code == 38 ? style_.foreground : style_.background;
                const auto form = values[++i];
                if (form == 5 && i + 1 < values.size()) {
                    const auto index = values[++i];
                    if (index <= 255) color = from_256(index);
                } else if (form == 2) {
                    if (i + 3 >= values.size()) break;
                    const auto red = values[++i], green = values[++i], blue = values[++i];
                    if (red <= 255 && green <= 255 && blue <= 255)
                        color = {static_cast<unsigned char>(red), static_cast<unsigned char>(green),
                                 static_cast<unsigned char>(blue)};
                }
            }
        }
    }
    void handle_escape(const std::string_view escape) {
        if (escape == "D") index();
        if (escape == "E") { move_to(row_, 0); index(); }
        if (escape == "M") {
            if (row_ == top_) { cells_.scroll(top_, bottom_, -1); move_to(row_, column_); }
            else move_to(step(row_, 1), column_);
        }
        if (escape == "7") saved_ = {row_, column_};
        if (escape == "8") move_to(saved_.first, saved_.second);
    }
    void modes(const std::vector<unsigned>& values, const bool enabled) {
        for (const auto mode : values) {
            if (mode == 7) { autowrap_ = enabled; wrap_pending_ = false; }
            else if (mode == 9 || mode == 1000 || mode == 1002 || mode == 1003) {
                if (enabled || mouse_reporting_ == mode) mouse_reporting_ = enabled ? mode : 0;
            } else if (mode == 1049 && enabled && !primary_) {
                auto alternate = ttytk::CellBuffer(cells_.rows(), cells_.columns());
                primary_ = std::move(cells_);
                primary_cursor_ = {row_, column_};
                cells_ = std::move(alternate);
                top_ = 0; bottom_ = cells_.rows();
                move_to(0, 0);
            } else if (mode == 1049 && !enabled && primary_) {
                cells_ = std::move(*primary_);
                primary_.reset();
                top_ = 0; bottom_ = cells_.rows();
                move_to(primary_cursor_.first, primary_cursor_.second);
            }
        }
    }
    void handle_csi(const std::string_view sequence) {
        if (sequence.empty()) return;
        const char final = sequence.back();
        auto data = sequence.substr(0, sequence.size() - 1);
        const bool dec = data.starts_with('?');
        if (dec) data.remove_prefix(1);
        const auto parsed = parameters(data);
        if (!parsed) return;
        const auto& values = *parsed;
        const auto first = std::max(1U, values[0]);
        const auto second = values.size() < 2 ? 1U : std::max(1U, values[1]);
        if (dec) {
            if (final == 'h' || final == 'l') modes(values, final == 'h');
            return;
        }
        switch (final) {
        case 'A': move_to(step(row_, first), column_); break;
        case 'B': move_to(add(row_, first), column_); break;
        case 'C': move_to(row_, add(column_, first)); break;
        case 'D': move_to(row_, step(column_, first)); break;
        case 'E': move_to(add(row_, first), 0); break;
        case 'F': move_to(step(row_, first), 0); break;
        case 'G': move_to(row_, first - 1); break;
        case 'd': move_to(first - 1, column_); break;
        case 'H':
        case 'f': move_to(first - 1, second - 1); break;
        case 'J':
        case 'K': erase(final, values[0]); break;
        case 'm': rendition(values); break;
        case 'r': {
            const auto end = values.size() < 2 || values[1] == 0 ? cells_.rows() : values[1];
            if (first < end && end <= cells_.rows()) { top_ = first - 1; bottom_ = end; move_to(0, 0); }
            break;
        }
        case 's': saved_ = {row_, column_}; break;
        case 'u': move_to(saved_.first, saved_.second); break;
        default: break;
        }
    }
    void handle_osc(const std::string_view sequence) {
        const auto separator = sequence.find(';');
        if (separator == std::string_view::npos) return;
        const std::string command{sequence.substr(0, separator)};
        const std::string payload{sequence.substr(separator + 1)};
        if (auto callback = title_callback_; callback && (command == "0" || command == "2")) callback(payload);
        if (auto callback = cwd_callback_; callback && command == "7") callback(payload);
        // OSC 52 and OSC 8 are notifications only; the host owns clipboard and URL policy.
        if (auto callback = osc_callback_) callback(command, payload);
    }

    ttytk::Automaton parser_;
    ttytk::CellBuffer cells_;
    std::optional<ttytk::CellBuffer> primary_;
    std::pair<std::size_t, std::size_t> primary_cursor_{};
    std::pair<std::size_t, std::size_t> saved_{};
    std::optional<std::pair<std::size_t, std::size_t>> previous_glyph_;
    std::size_t top_{}, bottom_{}, row_{}, column_{}, utf8_length_{};
    std::string utf8_;
    bool autowrap_{true};
    bool wrap_pending_{};
    unsigned mouse_reporting_{};
    ttytk::Cell style_;
    mutable std::vector<std::string> text_;
    Scrollback scrollback_;
    std::function<void(std::string, std::string)> osc_callback_{};
    std::function<void(std::string)> title_callback_{}, cwd_callback_{};
};

/// Owns a child PTY and connects it to a streaming emulator.
class Terminal {
public:
    Terminal() = default;
    Terminal(const Terminal&) = delete;
    auto operator=(const Terminal&) -> Terminal& = delete;
    ~Terminal() { close(); }

    [[nodiscard]] auto spawn(const std::vector<std::string>& command) -> bool {
        if (command.empty() || command.front().empty()) return false;
        std::vector<std::string> arguments(command.begin() + 1, command.end());
        auto process = ttytk::PTY::fork(command.front(), arguments);
        if (!process) return false;
        const int fd = process.value().master_fd();
        const int flags = ::fcntl(fd, F_GETFL);
        if (!process.value().resize(size_) || flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            stop_child(process.value());
            return false;
        }
        close();
        emulator_.reset();
        pty_ = std::move(process).value();
        return true;
    }

    void attach(loop::Loop& loop) {
        watch_.cancel();
        if (pty_) watch_ = loop.watch(pty_->master_fd(), loop::Readable, [this](const loop::Event&) { pump(); });
    }
    [[nodiscard]] auto resize(const ttytk::winsize::Size size) -> bool {
        if (size.rows > std::numeric_limits<unsigned short>::max() ||
            size.columns > std::numeric_limits<unsigned short>::max() ||
            size.pixel_width > std::numeric_limits<unsigned short>::max() ||
            size.pixel_height > std::numeric_limits<unsigned short>::max()) return false;
        if (pty_ && !pty_->resize(size)) return false;
        size_ = size;
        emulator_.resize(size.rows, size.columns);
        return true;
    }
    void on_title(std::function<void(std::string)> callback) { emulator_.on_title(std::move(callback)); }
    void on_cwd(std::function<void(std::string)> callback) { emulator_.on_cwd(std::move(callback)); }
    void on_osc(std::function<void(std::string, std::string)> callback) { emulator_.on_osc(std::move(callback)); }
    [[nodiscard]] auto emulator() noexcept -> Emulator& { return emulator_; }
    [[nodiscard]] auto write(const std::string_view text) -> bool {
        if (!pty_) return false;
        const auto written = pty_->write(text);
        return written && written.value() == text.size();
    }
    /// Forward an already encoded terminal mouse report only while requested by the child.
    [[nodiscard]] auto write_mouse(const std::string_view report) -> bool {
        return emulator_.mouse_reporting() && write(report);
    }
    void pump() {
        if (!pty_) return;
        char buffer[4096];
        const auto result = pty_->read(buffer, sizeof(buffer));
        if (result && result.value() != 0) emulator_.feed({buffer, result.value()});
        else if (result || (result.error().code != EAGAIN && result.error().code != EWOULDBLOCK &&
                            result.error().code != EINTR)) {
            watch_.cancel();
            (void)pty_->wait(true);
        }
    }
    /// Close the PTY and terminate/reap any child still owned by this terminal.
    void close() noexcept {
        watch_.cancel();
        if (pty_) stop_child(*pty_);
        pty_.reset();
    }

private:
    static void stop_child(ttytk::PTY& pty) noexcept {
        if (pty.child_pid() <= 0) return;
        const auto waited = pty.wait(true);
        if (waited && !waited.value()) {
            const auto pid = pty.child_pid();
            (void)::kill(-pid, SIGKILL);
            (void)::kill(pid, SIGKILL);
            (void)pty.wait();
        }
    }
    std::optional<ttytk::PTY> pty_;
    ttytk::winsize::Size size_{24, 80};
    Emulator emulator_;
    loop::FdWatch watch_;
};

inline void hook_osc(Emulator& emulator, std::function<void(std::string, std::string)> callback) {
    emulator.on_osc(std::move(callback));
}

}  // namespace ttyutils::term
