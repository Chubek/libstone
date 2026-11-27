#pragma once

#include <TTyTk/TTyTk-ColorTools.hpp>
#include <TTyUtils/TTyUtils-Config.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ttyutils::theme {

struct Style {
    std::optional<ttytk::color::Rgb> foreground{};
    std::optional<ttytk::color::Rgb> background{};
    std::uint32_t attributes{};

    [[nodiscard]] friend auto operator==(const Style&, const Style&) -> bool = default;
};

class Palette {
public:
    void set(std::string name, ttytk::color::Rgb value) { colors_[std::move(name)] = value; }
    [[nodiscard]] auto get(const std::string_view name) const -> std::optional<ttytk::color::Rgb> {
        const auto found = colors_.find(std::string(name));
        return found == colors_.end() ? std::nullopt : std::optional{found->second};
    }
    [[nodiscard]] auto quantize(const ttytk::color::Rgb color, const unsigned colors = 256) const noexcept
        -> ttytk::color::Rgb {
        if (colors <= 16) return ttytk::color::from_16(ttytk::color::to_16(color));
        if (colors <= 256) return ttytk::color::from_256(ttytk::color::to_256(color));
        return color;
    }

private:
    std::unordered_map<std::string, ttytk::color::Rgb> colors_;
};

class Theme {
public:
    /// Returns the cascade of generic roles, dotted parent roles, and active variant overrides.
    [[nodiscard]] auto role(const std::string_view name) const -> Style {
        Style result;
        if (const auto base = roles_.find(""); base != roles_.end()) merge(result, base->second);
        const auto& variant = dark_ ? dark_roles_ : light_roles_;
        if (const auto base = variant.find(""); base != variant.end()) merge(result, base->second);
        for (std::size_t from = 0; !name.empty();) {
            const auto end = name.find('.', from);
            const auto current = std::string(name.substr(0, end));
            if (const auto found = roles_.find(current); found != roles_.end()) merge(result, found->second);
            if (const auto found = variant.find(current); found != variant.end()) merge(result, found->second);
            if (end == std::string_view::npos) break;
            from = end + 1;
        }
        return result;
    }
    void set_role(std::string name, Style style) { roles_[std::move(name)] = style; }
    void set_variant(const bool dark, std::string name, Style style) {
        (dark ? dark_roles_ : light_roles_)[std::move(name)] = style;
    }
    [[nodiscard]] auto role(const std::string_view name, const unsigned colors) const -> Style {
        auto result = role(name);
        if (result.foreground) result.foreground = palette_.quantize(*result.foreground, colors);
        if (result.background) result.background = palette_.quantize(*result.background, colors);
        return result;
    }
    [[nodiscard]] auto palette() const noexcept -> const Palette& { return palette_; }
    [[nodiscard]] auto palette() noexcept -> Palette& { return palette_; }
    void set_dark_mode(const bool value) noexcept { dark_ = value; }
    [[nodiscard]] auto dark_mode() const noexcept -> bool { return dark_; }

    /// Applies an OSC 11 response body, e.g. `11;rgb:0000/0000/0000`.
    /// OSC 10 is the foreground query and does not select the theme variant.
    auto apply_background(std::string_view body) noexcept -> bool {
        if (!body.starts_with("11;")) return false;
        body.remove_prefix(3);
        auto rgb = ttytk::color::parse(body);
        if (!rgb && body.starts_with("rgb:")) {
            body.remove_prefix(4);
            ttytk::color::Rgb parsed;
            for (auto* channel : {&parsed.red, &parsed.green, &parsed.blue}) {
                const auto slash = body.find('/');
                if ((channel == &parsed.blue) != (slash == std::string_view::npos)) return false;
                const auto text = body.substr(0, slash);
                if (text.empty() || text.size() > 4) return false;
                unsigned value{};
                const auto end = text.data() + text.size();
                const auto converted = std::from_chars(text.data(), end, value, 16);
                if (converted.ec != std::errc{} || converted.ptr != end) return false;
                const auto max = (1U << (4 * text.size())) - 1;
                *channel = static_cast<std::uint8_t>((value * 255 + max / 2) / max);
                if (slash != std::string_view::npos) body.remove_prefix(slash + 1);
            }
            rgb = parsed;
        }
        if (!rgb) return false;
        dark_ = ttytk::color::luminance(*rgb) < 0.5;
        return true;
    }

private:
    static void merge(Style& base, const Style& override_style) {
        if (override_style.foreground) base.foreground = override_style.foreground;
        if (override_style.background) base.background = override_style.background;
        base.attributes |= override_style.attributes;
    }
    std::unordered_map<std::string, Style> roles_;
    std::unordered_map<std::string, Style> dark_roles_, light_roles_;
    Palette palette_;
    bool dark_{};
};

[[nodiscard]] inline auto load(const std::filesystem::path& path) -> Theme {
    Theme result;
    const auto settings = config::Store::load_file(path);
    result.set_dark_mode(settings.get<bool>("dark", settings.get<int>("dark") == 1));
    for (const auto& [name, value] : settings.values()) {
        if (!std::string_view{name}.starts_with("palette.")) continue;
        if (const auto color = ttytk::color::parse(value.as<std::string>()))
            result.palette().set(name.substr(8), *color);
    }
    const auto get_color = [&result](const config::Value& value) {
        const auto text = value.as<std::string>();
        return text.starts_with('$') ? result.palette().get(std::string_view{text}.substr(1)) :
                                       ttytk::color::parse(text);
    };
    std::unordered_map<std::string, Style> common, dark, light;
    for (const auto& [name, value] : settings.values()) {
        auto key = std::string_view{name};
        auto* roles = &common;
        if (key.starts_with("palette.")) continue;
        if (key.starts_with("dark.")) { key.remove_prefix(5); roles = &dark; }
        else if (key.starts_with("light.")) { key.remove_prefix(6); roles = &light; }
        if (key.starts_with("roles.")) key.remove_prefix(6);
        const auto dot = key.rfind('.');
        if (dot == std::string_view::npos && key == "dark") continue;
        auto role_name = dot == std::string_view::npos ? std::string{} : std::string(key.substr(0, dot));
        if (role_name == "default") role_name.clear();
        const auto field = dot == std::string_view::npos ? key : key.substr(dot + 1);
        if (field == "attributes") {
            (*roles)[role_name].attributes = value.as<std::uint32_t>();
        } else if (field == "fg" || field == "foreground") {
            if (const auto color = get_color(value)) (*roles)[role_name].foreground = color;
        } else if (field == "bg" || field == "background") {
            if (const auto color = get_color(value)) (*roles)[role_name].background = color;
        }
    }
    for (const auto& [name, style] : common) result.set_role(name, style);
    for (const auto& [name, style] : dark) result.set_variant(true, name, style);
    for (const auto& [name, style] : light) result.set_variant(false, name, style);
    return result;
}

// NOTE(agent): The manifest exposes role/dark_mode as namespace symbols while
// the example uses Theme members. Both forms are supplied for compatibility.
[[nodiscard]] inline auto role(const Theme& theme, const std::string_view name) -> Style {
    return theme.role(name);
}
[[nodiscard]] inline auto dark_mode(const Theme& theme) noexcept -> bool {
    return theme.dark_mode();
}

}  // namespace ttyutils::theme
