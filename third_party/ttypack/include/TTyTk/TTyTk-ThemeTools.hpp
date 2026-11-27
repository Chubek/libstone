#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ttytk::theme {

/** @brief A named ANSI palette and the terminal's default text colors. */
struct Theme {
    std::string name{};
    std::array<color::Rgb, 16> palette{};
    color::Rgb foreground{229, 229, 229};
    color::Rgb background{};
};

namespace detail {

[[nodiscard]] inline auto find_builtin(std::string_view name) -> std::optional<Theme> {
    if (name == "default") {
        return Theme{"default", {{{0, 0, 0}, {205, 49, 49}, {13, 188, 121}, {229, 229, 16},
                                  {36, 114, 200}, {188, 63, 188}, {17, 168, 205}, {229, 229, 229},
                                  {102, 102, 102}, {241, 76, 76}, {35, 209, 139}, {245, 245, 67},
                                  {59, 142, 234}, {214, 112, 214}, {41, 184, 219}, {255, 255, 255}}}};
    }
    if (name == "solarized-dark" || name == "solarized-light") {
        const bool light = name == "solarized-light";
        return Theme{std::string{name}, {{{7, 54, 66}, {220, 50, 47}, {133, 153, 0}, {181, 137, 0},
                                         {38, 139, 210}, {211, 54, 130}, {42, 161, 152}, {238, 232, 213},
                                         {0, 43, 54}, {203, 75, 22}, {88, 110, 117}, {101, 123, 131},
                                         {131, 148, 150}, {108, 113, 196}, {147, 161, 161}, {253, 246, 227}}},
                     light ? color::Rgb{101, 123, 131} : color::Rgb{131, 148, 150},
                     light ? color::Rgb{253, 246, 227} : color::Rgb{0, 43, 54}};
    }
    return std::nullopt;
}

[[nodiscard]] inline auto trim(std::string_view text) -> std::string_view {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

[[nodiscard]] inline auto scalar(std::string_view value) -> std::string_view {
    value = trim(value);
    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
        const auto close = value.find(value.front(), 1);
        if (close == std::string_view::npos) return {};
        const auto suffix = trim(value.substr(close + 1));
        if (!suffix.empty() && suffix.front() != '#') return {};
        return value.substr(1, close - 1);
    }
    const auto comment = value.find_first_of(" \t");
    if (comment != std::string_view::npos) {
        const auto suffix = trim(value.substr(comment));
        if (!suffix.empty() && suffix.front() == '#') return value.substr(0, comment);
        const auto marker = value.find(" #", comment);
        if (marker != std::string_view::npos) return trim(value.substr(0, marker));
    }
    return value;
}

} // namespace detail

/** @brief Look up a bundled palette, falling back to the default for unknown names. */
[[nodiscard]] inline auto builtin(std::string_view name) -> Theme {
    if (auto selected = detail::find_builtin(name)) return *selected;
    return *detail::find_builtin("default");
}

/** @brief Load a built-in name or a Base16 YAML subset with base00 through base0F colors.
 * Accepts bare or quoted RGB hex values (with an optional leading '#'), a scheme/name,
 * and the usual palette wrapper. Returns nullopt for missing or invalid colors.
 */
[[nodiscard]] inline auto load(std::string_view source) -> std::optional<Theme> {
    if (auto selected = detail::find_builtin(detail::trim(source))) return selected;
    std::array<std::optional<color::Rgb>, 16> base{};
    std::string name;
    while (!source.empty()) {
        const auto eol = source.find('\n');
        const auto line = detail::trim(source.substr(0, eol));
        source = eol == std::string_view::npos ? std::string_view{} : source.substr(eol + 1);
        if (line.empty() || line.front() == '#') continue;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const auto key = detail::trim(line.substr(0, colon));
        const auto value = detail::scalar(line.substr(colon + 1));
        if (key == "scheme" || key == "name") {
            name = value;
        } else if (key.size() == 6 && key.starts_with("base0")) {
            const auto digit = key.back();
            const auto index = digit >= '0' && digit <= '9' ? digit - '0'
                             : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10
                             : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1;
            if (index < 0 || base[static_cast<std::size_t>(index)]) return std::nullopt;
            if (value.size() == 6) base[static_cast<std::size_t>(index)] = color::parse('#' + std::string{value});
            else if (value.size() == 7 && value.front() == '#')
                base[static_cast<std::size_t>(index)] = color::parse(value);
            if (!base[static_cast<std::size_t>(index)]) return std::nullopt;
        }
    }
    for (const auto& slot : base) if (!slot) return std::nullopt;
    constexpr std::array ansi{0x0, 0x8, 0xB, 0xA, 0xD, 0xE, 0xC, 0x5,
                              0x3, 0x8, 0xB, 0xA, 0xD, 0xE, 0xC, 0x7};
    Theme result{std::move(name), {}, *base[5], *base[0]};
    for (std::size_t i = 0; i < ansi.size(); ++i) result.palette[i] = *base[ansi[i]];
    return result;
}

/** @brief Build xterm OSC 4, 10, and 11 commands (without writing to a terminal). */
[[nodiscard]] inline auto apply(const Theme& theme) -> std::string {
    constexpr char digits[] = "0123456789abcdef";
    const auto rgb = [&](color::Rgb color) {
        std::string result = "rgb:00/00/00";
        const auto component = [&](std::size_t offset, unsigned value) {
            result[offset] = digits[value >> 4U];
            result[offset + 1] = digits[value & 0xFU];
        };
        component(4, color.red);
        component(7, color.green);
        component(10, color.blue);
        return result;
    };
    std::string out = std::string{esc::osc} + '4';
    for (std::size_t i = 0; i < theme.palette.size(); ++i)
        out += ';' + std::to_string(i) + ';' + rgb(theme.palette[i]);
    out += "\x1b\\";
    out += std::string{esc::osc} + "10;" + rgb(theme.foreground) + "\x1b\\";
    out += std::string{esc::osc} + "11;" + rgb(theme.background) + "\x1b\\";
    return out;
}

} // namespace ttytk::theme
