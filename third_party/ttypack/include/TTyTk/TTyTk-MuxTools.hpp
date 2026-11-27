#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-PsuedoTerm.hpp>

#include <cerrno>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace ttytk {

/** Orientation of a pane divider. */
enum class Window { Vertical, Horizontal };

/** Manage terminal sessions and route I/O for the selected pane. */
class Mux {
public:
    struct Session {
        CellBuffer screen{};
        PTY terminal{};

        Session() = default;
        Session(Session&&) noexcept = default;
        auto operator=(Session&&) noexcept -> Session& = default;
        Session(const Session&) = delete;
        auto operator=(const Session&) -> Session& = delete;
    };

    struct Split {
        std::size_t parent;
        Window direction;
    };

    [[nodiscard]] inline auto add_window(Session session) -> std::size_t {
        return add(std::move(session), std::nullopt);
    }

    [[nodiscard]] inline auto add_window() -> std::size_t { return add_window(Session{}); }

    [[nodiscard]] inline auto split(const Window direction) -> std::size_t {
        return split(direction, Session{});
    }

    /** Add a pane beside the active session, or start a root window if empty. */
    [[nodiscard]] inline auto split(const Window direction, Session session) -> std::size_t {
        if (sessions_.empty()) return add_window(std::move(session));
        return add(std::move(session), Split{active_, direction});
    }

    [[nodiscard]] inline auto split_info(const std::size_t index) const noexcept -> std::optional<Split> {
        return index < sessions_.size() ? sessions_[index].split : std::nullopt;
    }

    [[nodiscard]] inline auto active_window() noexcept -> Session* {
        return sessions_.empty() ? nullptr : &sessions_[active_].session;
    }

    [[nodiscard]] inline auto next_window() noexcept -> Session* {
        if (sessions_.empty()) return nullptr;
        active_ = (active_ + 1) % sessions_.size();
        return active_window();
    }

    /** Write to the active PTY; return the number of bytes it accepted. */
    [[nodiscard]] inline auto send_input(const std::string_view bytes) -> Result<std::size_t> {
        auto* selected = active_window();
        if (!selected) return Error{ENOENT, "no active terminal"};
        return selected->terminal.write(bytes);
    }

    using OutputHandler = std::function<void(CellBuffer&, std::string_view)>;

    /** Read one chunk from the active PTY and apply it using the caller's screen parser. */
    [[nodiscard]] inline auto receive_output(const OutputHandler& handler) -> Result<std::size_t> {
        auto* selected = active_window();
        if (!selected) return Error{ENOENT, "no active terminal"};
        if (!handler) return Error{EINVAL, "no terminal output handler"};
        char bytes[4096];
        const auto size = selected->terminal.read(bytes, sizeof(bytes));
        if (size && size.value() != 0)
            handler(selected->screen, std::string_view{bytes, size.value()});
        return size;
    }

private:
    struct Pane {
        Session session;
        std::optional<Split> split;
    };

    [[nodiscard]] inline auto add(Session session, const std::optional<Split> split) -> std::size_t {
        sessions_.push_back({std::move(session), split});
        active_ = sessions_.size() - 1;
        return active_;
    }

    std::deque<Pane> sessions_{};
    std::size_t active_{};
};

}  // namespace ttytk
