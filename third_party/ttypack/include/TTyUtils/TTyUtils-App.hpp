#pragma once

#include <TTyTk/TTyTk-CapTools.hpp>
#include <TTyUtils/TTyUtils-EventLoop.hpp>
#include <TTyUtils/TTyUtils-Focus.hpp>
#include <TTyUtils/TTyUtils-Theme.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <utility>
#include <vector>

namespace ttyutils::app {

struct Options {
    bool mouse{};
    bool alternate_screen{true};
    bool raw_input{true};
    /// File descriptors are borrowed, and may be -1 for headless execution.
    int input_fd{STDIN_FILENO};
    int output_fd{STDOUT_FILENO};
    /// Probe the terminal for capabilities and its default background.
    bool negotiate{};
};

class Screen {
public:
    Screen(const std::size_t rows = 24, const std::size_t columns = 80)
        : rows_(rows), columns_(columns) {}

    void resize(const std::size_t rows, const std::size_t columns) noexcept {
        rows_ = rows;
        columns_ = columns;
    }
    [[nodiscard]] auto rows() const noexcept -> std::size_t { return rows_; }
    [[nodiscard]] auto columns() const noexcept -> std::size_t { return columns_; }
    void write(const std::string_view text) { output_ += text; }
    /// Most recent draw update (ANSI on a capable terminal; plain text headless).
    [[nodiscard]] auto output() const noexcept -> std::string_view { return output_; }
    void clear_output() { output_.clear(); }

private:
    std::size_t rows_;
    std::size_t columns_;
    std::string output_;
};

/// Coordinates a widget tree and a terminal session. Headless sessions retain
/// drawing output in screen() without writing terminal sequences.
class App {
public:
    explicit App(const Options options = {}) : options_(options) {}

    void set_root(std::unique_ptr<ui::Widget> root) {
        root_ = std::move(root);
        full_redraw_ = true;
    }
    void set_theme(theme::Theme theme) { theme_ = std::move(theme); full_redraw_ = true; }
    [[nodiscard]] auto theme() const noexcept -> const theme::Theme& { return theme_; }
    [[nodiscard]] auto screen() noexcept -> Screen& { return screen_; }
    [[nodiscard]] auto loop() noexcept -> loop::Loop& { return loop_; }

    /// Delivers a decoded input event, bubbling through ancestors within the
    /// active focus scope. Unhandled Tab/Shift-Tab traverses that scope.
    auto dispatch(const ttytk::input::Event& event) -> bool {
        if (!root_) return false;
        auto& scope = ui::FocusScope::within(*root_);
        ui::FocusChain focus(*root_);
        auto* target = focus.current();
        if (const auto* mouse = std::get_if<ttytk::input::Mouse>(&event)) {
            int x{}, y{};
            if (!ui::mouse_cell(*mouse, x, y)) return false;
            target = scope.hit_test(x, y).widget;
            if (!target) return false;
            if (mouse->pressed && !mouse->motion && mouse->button < 3) {
                for (auto* node = target; node; node = node->parent()) {
                    if (focus.focus(*node) || node == &scope) break;
                }
            }
        }
        for (auto* node = target ? target : &scope; node; node = node->parent()) {
            if (node->handle(event)) return true;
            if (node == &scope) break;
        }
        if (const auto* key = std::get_if<ttytk::input::Key>(&event); key && key->event_type != 3) {
            if (key->codepoint == U'\t' && (key->modifiers & ~1U) == 0)
                return ((key->modifiers & 1U) ? focus.prev() : focus.next()) != nullptr;
        }
        return false;
    }

    [[nodiscard]] auto run() -> int {
        if (quit_requested_ || suspended_ || running_) return exit_code_;
        TerminalSession terminal(options_);
        struct RunGuard {
            App& app;
            loop::FdWatch input;
            ~RunGuard() { input.cancel(); app.running_ = false; }
        } guard{*this, {}};
        running_ = true;
        if (::isatty(options_.input_fd)) {
            guard.input = loop_.watch(options_.input_fd, loop::Readable, [this, &guard](const loop::Event& event) {
                char bytes[4096];
                const auto length = ::read(event.fd, bytes, sizeof(bytes));
                if (length > 0) input_parser_.feed(bytes, static_cast<std::size_t>(length));
                else if (length == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    guard.input.cancel();
                    quit();
                }
            });
        }
        setup_input();
        colors_ = terminal.colors();
        if (terminal.controls() && options_.negotiate && ::isatty(options_.input_fd)) {
            terminal.write(ttytk::caps::request());
            terminal.write("\x1b]11;?\x1b\\");
        }
        full_redraw_ = true;
        redraw(terminal);
        while (running_ && !suspended_) {
            loop_.run_once();
            if (running_ && !suspended_) redraw(terminal);
        }
        return exit_code_;
    }

    void quit(const int code = 0) noexcept {
        exit_code_ = code;
        quit_requested_ = true;
        running_ = false;
    }
    void suspend() noexcept { suspended_ = true; }
    void resume() noexcept { suspended_ = false; }
    [[nodiscard]] auto suspended() const noexcept -> bool { return suspended_; }

private:
    class TerminalSession {
    public:
        explicit TerminalSession(const Options& options)
            : options_(options), terminal_output_(::isatty(options.output_fd)) {
            const auto* term = std::getenv("TERM");
            controls_ = terminal_output_ && (!term || std::string_view{term} != "dumb");
            if (options_.raw_input && ::tcgetattr(options_.input_fd, &previous_) == 0) {
                auto raw = previous_;
                ::cfmakeraw(&raw);
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                raw_ = ::tcsetattr(options_.input_fd, TCSANOW, &raw) == 0;
            }
            if (controls_) {
                // DECSET 1049 saves cursor and switches screen; SGR mode 1006
                // augments button-motion tracking (1002), it does not enable it.
                if (options_.alternate_screen) write("\x1b[?1049h");
                if (options_.mouse) write("\x1b[?1002h\x1b[?1006h");
            }
        }
        ~TerminalSession() {
            if (controls_) {
                write("\x1b[0m");
                if (options_.mouse) write("\x1b[?1006l\x1b[?1002l");
                if (options_.alternate_screen) write("\x1b[?1049l");
            }
            if (raw_) while (::tcsetattr(options_.input_fd, TCSANOW, &previous_) < 0 && errno == EINTR) {}
        }
        [[nodiscard]] auto controls() const noexcept -> bool { return controls_; }
        [[nodiscard]] auto unicode() const noexcept -> bool { return !terminal_output_ || controls_; }
        [[nodiscard]] auto colors() const noexcept -> unsigned {
            const auto* color_term = std::getenv("COLORTERM");
            const auto* term = std::getenv("TERM");
            if (color_term && (std::string_view{color_term} == "truecolor" || std::string_view{color_term} == "24bit"))
                return 1U << 24;
            return term && std::string_view(term).find("256color") != std::string_view::npos ? 256 : 16;
        }
        void update_size(Screen& screen) const noexcept {
            struct winsize size {};
            if (terminal_output_ && ::ioctl(options_.output_fd, TIOCGWINSZ, &size) == 0 &&
                size.ws_row && size.ws_col)
                screen.resize(size.ws_row, size.ws_col);
        }
        void write(std::string_view bytes) const noexcept {
            if (!terminal_output_) return;
            while (!bytes.empty()) {
                const auto written = ::write(options_.output_fd, bytes.data(), bytes.size());
                if (written > 0) bytes.remove_prefix(static_cast<std::size_t>(written));
                else if (written == 0 || errno != EINTR) break;
            }
        }
    private:
        const Options& options_;
        bool terminal_output_;
        bool controls_{};
        bool raw_{};
        struct termios previous_ {};
    };

    void setup_input() {
        input_parser_.reset();
        input_text_.clear();
        ss3_ = false;
        input_parser_.on_print = [this](const std::string_view bytes) {
            auto text = bytes;
            if (ss3_ && !text.empty()) {
                ss3_ = false;
                const auto sequence = std::string("\x1bO") + text.front();
                if (auto key = ttytk::input::decode_seq(sequence)) {
                    dispatch(key.value());
                    text.remove_prefix(1);
                } else dispatch(ttytk::input::Key{U'O', 2});
            }
            input_text_ += text;
            dispatch_printable(false);
        };
        input_parser_.on_control = [this](const unsigned char byte) {
            dispatch_printable(true);
            if (byte >= 1 && byte <= 26 && byte != 8 && byte != 9 && byte != 10 && byte != 13)
                dispatch(ttytk::input::Key{static_cast<char32_t>(U'a' + byte - 1), 4});
            else dispatch(ttytk::input::Key{static_cast<char32_t>(byte == 10 ? 13 : byte)});
        };
        input_parser_.on_csi = [this](const std::string_view sequence) {
            dispatch_printable(true);
            if (sequence == "Z") { dispatch(ttytk::input::Key{U'\t', 1}); return; }
            auto decoded = ttytk::input::decode_seq(std::string("\x1b[") + std::string(sequence));
            if (decoded) dispatch(decoded.value());
        };
        input_parser_.on_escape = [this](const std::string_view sequence) {
            dispatch_printable(true);
            if (sequence == "O") ss3_ = true;
            else if (sequence.size() == 1)
                dispatch(ttytk::input::Key{static_cast<unsigned char>(sequence.front()), 2});
        };
        input_parser_.on_dcs = [this](const std::string_view sequence) {
            dispatch_printable(true);
            if (options_.negotiate &&
                ttytk::caps::query("\x1bP" + std::string(sequence) + "\x1b\\").has_truecolor &&
                colors_ < (1U << 24)) {
                colors_ = 1U << 24;
                full_redraw_ = true;
            }
        };
        input_parser_.on_osc = [this](const std::string_view sequence) {
            dispatch_printable(true);
            const auto was_dark = theme_.dark_mode();
            if (options_.negotiate && theme_.apply_background(sequence) && was_dark != theme_.dark_mode())
                full_redraw_ = true;
        };
    }

    void dispatch_printable(const bool flush) {
        std::size_t offset = 0;
        while (offset < input_text_.size()) {
            const auto scalar = ttytk::detail::decode_utf8_scalar(input_text_, offset);
            const auto lead = static_cast<unsigned char>(input_text_[offset]);
            const auto expected = lead >= 0xc2 && lead <= 0xf4 ?
                static_cast<std::size_t>(lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4) : 1;
            if (!flush && !scalar.valid && scalar.length < expected && offset + scalar.length == input_text_.size())
                break;
            dispatch(ttytk::input::Key{scalar.value});
            offset += scalar.length;
        }
        input_text_.erase(0, offset);
    }

    void redraw(const TerminalSession& terminal) {
        terminal.update_size(screen_);
        const auto as_int = [](const std::size_t value) {
            return static_cast<int>(std::min(value, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        };
        const ui::Rect bounds{0, 0, as_int(screen_.columns()), as_int(screen_.rows())};
        const auto clear_screen = full_redraw_ || bounds != previous_bounds_;
        if (!clear_screen && (!root_ || root_->damage().empty())) return;
        std::vector<std::pair<int, int>> dirty_rows;
        if (terminal.controls() && !clear_screen && root_) {
            for (const auto& damage : root_->damage()) {
                const auto affected = damage.intersect(bounds);
                if (!affected.empty()) dirty_rows.emplace_back(affected.y, affected.y + affected.height);
            }
            std::sort(dirty_rows.begin(), dirty_rows.end());
        }
        ui::Canvas canvas(bounds, terminal.unicode());
        if (root_) {
            root_->set_rect(bounds);
            root_->render(canvas);
        }
        auto output = std::string{};
        if (terminal.controls()) {
            if (clear_screen) output = "\x1b[0m\x1b[2J";
            else {
                int cleared = 0;
                for (const auto& [first, last] : dirty_rows) {
                    for (auto row = std::max(first, cleared); row < last; ++row)
                        output += "\x1b[" + std::to_string(row + 1) + ";1H\x1b[0m\x1b[2K";
                    cleared = std::max(cleared, last);
                }
            }
        }
        for (const auto& text : canvas.texts()) {
            if (terminal.controls()) {
                if (!clear_screen && std::none_of(dirty_rows.begin(), dirty_rows.end(), [&text](const auto& range) {
                        return text.y >= range.first && text.y < range.second;
                    })) continue;
                output += "\x1b[" + std::to_string(text.y + 1) + ";" + std::to_string(text.x + 1) + "H\x1b[0m";
                for (unsigned attribute = 0; attribute < 9; ++attribute)
                    if ((text.style.attributes & (1U << attribute)) != 0)
                        output += "\x1b[" + std::to_string(attribute + 1) + "m";
                append_color(output, text.style.foreground, 38);
                append_color(output, text.style.background, 48);
            }
            output += text.value;
        }
        terminal.write(output);
        screen_.clear_output();
        screen_.write(output);
        previous_bounds_ = bounds;
        full_redraw_ = false;
    }

    void append_color(std::string& output, const std::optional<ttytk::color::Rgb> value, const int plane) const {
        if (!value) return;
        if (colors_ <= 16) {
            const auto index = ttytk::color::to_16(*value);
            output += "\x1b[" + std::to_string(plane - 8 + (index >= 8 ? 60 : 0) + index % 8) + "m";
        } else if (colors_ <= 256) {
            output += "\x1b[" + std::to_string(plane) + ";5;" + std::to_string(ttytk::color::to_256(*value)) + "m";
        } else {
            output += "\x1b[" + std::to_string(plane) + ";2;" + std::to_string(value->red) + ";" +
                      std::to_string(value->green) + ";" + std::to_string(value->blue) + "m";
        }
    }

    Options options_;
    Screen screen_;
    loop::Loop loop_;
    theme::Theme theme_;
    std::unique_ptr<ui::Widget> root_;
    ttytk::Automaton input_parser_;
    std::string input_text_;
    ui::Rect previous_bounds_{};
    unsigned colors_{16};
    bool full_redraw_{true};
    bool ss3_{};
    int exit_code_{};
    bool running_{};
    bool suspended_{};
    bool quit_requested_{};
};

inline void quit(App& application, const int code = 0) noexcept {
    application.quit(code);
}

inline void suspend(App& application) noexcept {
    application.suspend();
}

}  // namespace ttyutils::app
