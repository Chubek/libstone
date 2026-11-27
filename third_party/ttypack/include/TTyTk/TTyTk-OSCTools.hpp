#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <charconv>
#include <string>
#include <string_view>

namespace ttytk::osc {

namespace detail {

[[nodiscard]] inline auto command(const std::string_view body) -> std::string {
    return std::string{esc::osc} + std::string{body} + std::string{esc::st};
}

}  // namespace detail

/** @brief Build an xterm OSC 2 window title, omitting control characters. */
[[nodiscard]] inline auto set_title(const std::string_view title) -> std::string {
    return detail::command("2;" + esc::detail::printable_utf8(title));
}

/** @brief Build OSC 8 around plain display text, omitting control characters. */
[[nodiscard]] inline auto hyperlink(const std::string_view uri, const std::string_view text) -> std::string {
    return esc::Hyperlink::open(uri) + esc::detail::printable_utf8(text) + esc::Hyperlink::close();
}

/** @brief Build an OSC 9 desktop notification, omitting control characters. */
[[nodiscard]] inline auto desktop_notify(const std::string_view message) -> std::string {
    return detail::command("9;" + esc::detail::printable_utf8(message));
}

/** @brief Build an xterm OSC 4 query for a palette entry. */
[[nodiscard]] inline auto palette_query(const unsigned index) -> std::string {
    return detail::command("4;" + std::to_string(index) + ";?");
}

/** @brief Build an xterm OSC 4 update; color is an xterm color specification. */
[[nodiscard]] inline auto palette_set(const unsigned index, const std::string_view color)
    -> Result<std::string> {
    if (color.empty() || color == "?" || color.find(';') != std::string_view::npos ||
        esc::detail::printable_utf8(color) != color) {
        return Error{1, "invalid palette color"};
    }
    return detail::command("4;" + std::to_string(index) + ';' + std::string{color});
}

/** @brief An OSC command number and borrowed arguments after the first semicolon. */
struct Command {
    unsigned code{};
    std::string_view arguments{};
};

/** @brief Parse an OSC body from Automaton::on_osc (without introducer or terminator). */
[[nodiscard]] inline auto parse(const std::string_view body) -> Result<Command> {
    const auto separator = body.find(';');
    const auto code = body.substr(0, separator);
    Command command;
    if (code.empty()) return Error{1, "missing OSC code"};
    const auto converted = std::from_chars(code.data(), code.data() + code.size(), command.code);
    if (converted.ec != std::errc{} || converted.ptr != code.data() + code.size()) {
        return Error{1, "invalid OSC code"};
    }
    if (separator != std::string_view::npos) command.arguments = body.substr(separator + 1);
    return command;
}

}  // namespace ttytk::osc
