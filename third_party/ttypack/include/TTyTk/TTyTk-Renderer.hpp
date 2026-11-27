#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-Damage.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>
#include <TTyTk/TTyTk-TermMode.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

namespace ttytk {

/** @brief Write changed terminal cells using CUP and SGR, caching the displayed frame. */
class Renderer {
public:
    /** @brief Compare every cell; no bytes are written for an unchanged frame. */
    void render(const CellBuffer& buffer, std::ostream& output) { render_frame(buffer, output, nullptr); }

    /** @brief Compare only marked cells and their wide-glyph partners.
     * The first frame, a resize, or switching terminals always triggers a full render.
     * The caller is responsible for clearing damage after a successful frame.
     */
    void render(const CellBuffer& buffer, std::ostream& output, const Damage& damage) {
        render_frame(buffer, output, &damage);
    }

    void flush(std::ostream& output) {
        try {
            output.flush();
        } catch (...) {
            reset_cache();
            throw;
        }
        if (!output) reset_cache();
    }

    /** @brief Request DECAWM (automatic wrapping) on or off. */
    void set_terminal_state(bool enabled) noexcept { auto_wrap_ = enabled; }

    /** @brief Request the supported DEC private modes for the next render. */
    void set_terminal_state(const TermMode& modes) noexcept {
        for (std::size_t i = 0; i < supported_modes_.size(); ++i)
            desired_modes_[i] = modes.test(supported_modes_[i]);
        auto_wrap_.reset();
    }

    /** @brief Forget terminal state after external output or transport changes. */
    void reset_cache() noexcept {
        cached_.reset();
        destination_ = nullptr;
        sent_modes_ = {};
    }

private:
    struct Style {
        std::uint32_t attributes;
        color::Rgb foreground, background;
    };

    static constexpr std::array supported_modes_{
        Mode::AltScreen, Mode::CursorKeys, Mode::AutoWrap, Mode::CursorVisible,
        Mode::MouseX10, Mode::Mouse, Mode::MouseHighlight, Mode::MouseButtonEvent,
        Mode::MouseAnyEvent, Mode::FocusReport, Mode::MouseUtf8, Mode::MouseSgr,
        Mode::MouseUrxvt, Mode::MouseSgrPixels, Mode::BracketedPaste,
    };
    using ModeStates = std::array<std::optional<bool>, supported_modes_.size()>;

    static void append_style(std::string& bytes, std::optional<Style>& previous, const Cell& cell) {
        const Style style{cell.attributes & 0x1ffU, cell.foreground, cell.background};
        std::string codes;
        const auto add = [&](unsigned code) {
            if (!codes.empty()) codes.push_back(';');
            codes += std::to_string(code);
        };
        auto active = previous ? previous->attributes : 0;
        if (!previous) add(0);
        const auto drop = [&](std::uint32_t mask, unsigned restore) {
            if ((active & mask & ~style.attributes) != 0) {
                add(restore);
                active &= ~mask;
            }
        };
        drop(0x003, 22); // bold and faint share an SGR reset
        drop(0x004, 23);
        drop(0x008, 24);
        drop(0x030, 25); // slow and rapid blink share an SGR reset
        drop(0x040, 27);
        drop(0x080, 28);
        drop(0x100, 29);
        for (unsigned i = 0; i < 9; ++i) {
            if ((style.attributes & ~active) & (1U << i)) add(i + 1);
        }
        const auto rgb = [&](unsigned target, color::Rgb value) {
            add(target);
            add(2);
            add(value.red);
            add(value.green);
            add(value.blue);
        };
        if (!previous || previous->foreground != style.foreground) rgb(38, style.foreground);
        if (!previous || previous->background != style.background) rgb(48, style.background);
        if (!codes.empty()) bytes += std::string{esc::csi} + codes + 'm';
        previous = style;
    }

    void render_frame(const CellBuffer& buffer, std::ostream& output, const Damage* damage) {
        if (destination_ != &output) reset_cache();
        if (!output) {
            reset_cache();
            return;
        }

        std::string bytes;
        auto modes = sent_modes_;
        bool repaint = !cached_ || cached_->rows() != buffer.rows() ||
                       cached_->columns() != buffer.columns();
        // Disable old modes before enabling new ones, notably for mouse protocols.
        for (const bool enabled : {false, true}) {
            for (std::size_t i = 0; i < supported_modes_.size(); ++i) {
                const auto mode = supported_modes_[i];
                const auto state = mode == Mode::AutoWrap && auto_wrap_ ? auto_wrap_ : desired_modes_[i];
                if (state && *state == enabled && state != modes[i]) {
                    bytes += std::string{esc::csi} + '?' + std::to_string(static_cast<unsigned>(mode)) +
                             (enabled ? 'h' : 'l');
                    modes[i] = state;
                    if (mode == Mode::AltScreen) repaint = true;
                }
            }
        }
        if (cached_ && (buffer.rows() < cached_->rows() || buffer.columns() < cached_->columns())) {
            bytes += esc::sgr({0});
            bytes += esc::clear();
        }

        Damage scan;
        if (!repaint && damage && buffer.columns() != 0) {
            const auto continuation = [&](auto row, auto column) {
                return buffer.at(row, column).continuation || cached_->at(row, column).continuation;
            };
            for (auto [row, first, last] : damage->regions()) {
                if (row >= buffer.rows() || first >= buffer.columns()) continue;
                last = std::min(last, buffer.columns() - 1);
                while (first > 0 && continuation(row, first)) --first;
                while (last + 1 < buffer.columns() && continuation(row, last + 1)) ++last;
                scan.mark(row, first, last);
            }
        }

        std::optional<Style> style;
        const auto render_span = [&](std::size_t row, std::size_t first, std::size_t last) {
            bool in_run = false;
            for (auto col = first; col <= last;) {
                const auto& cell = buffer.at(row, col);
                if (cell.continuation) {
                    ++col;
                    continue;
                }
                const auto wide = col + 1 < buffer.columns() && buffer.at(row, col + 1).continuation;
                if (repaint || cell != cached_->at(row, col) ||
                    (wide && buffer.at(row, col + 1) != cached_->at(row, col + 1))) {
                    if (!in_run) bytes += esc::cursor_to(static_cast<unsigned>(row + 1),
                                                        static_cast<unsigned>(col + 1));
                    append_style(bytes, style, cell);
                    bytes += cell.grapheme.empty() ? " " : cell.grapheme;
                    in_run = true;
                } else {
                    in_run = false;
                }
                col += wide ? 2U : 1U;
            }
        };
        if (repaint || !damage) {
            if (buffer.columns() != 0) {
                for (std::size_t row = 0; row < buffer.rows(); ++row)
                    render_span(row, 0, buffer.columns() - 1);
            }
        } else {
            for (const auto [row, first, last] : scan.regions()) render_span(row, first, last);
        }
        if (style) bytes += esc::sgr({0});

        try {
            if (!bytes.empty()) output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) {
                reset_cache();
                return;
            }
            if (repaint || !damage) cached_ = buffer;
            else {
                for (const auto [row, first, last] : scan.regions()) {
                    for (auto col = first; col <= last; ++col) cached_->at(row, col) = buffer.at(row, col);
                }
            }
        } catch (...) {
            reset_cache();
            throw;
        }
        destination_ = &output;
        sent_modes_ = modes;
    }

    std::optional<CellBuffer> cached_{};
    std::ostream* destination_{};
    std::optional<bool> auto_wrap_{};
    ModeStates desired_modes_{}, sent_modes_{};
};

} // namespace ttytk
