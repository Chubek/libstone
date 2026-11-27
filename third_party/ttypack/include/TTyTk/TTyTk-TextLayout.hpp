#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CharWidth.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ttytk::layout {

namespace detail {

[[nodiscard]] constexpr auto hard_break(std::string_view cluster) noexcept -> bool {
    return cluster == "\n" || cluster == "\r" || cluster == "\r\n" ||
           cluster == "\xC2\x85" || cluster == "\xE2\x80\xA8" || cluster == "\xE2\x80\xA9";
}

[[nodiscard]] constexpr auto word_space(std::string_view cluster) noexcept -> bool {
    const auto scalar = ttytk::detail::decode_utf8_scalar(cluster, 0);
    if (scalar.length != cluster.size()) return false;
    const auto cp = scalar.value;
    return cp == U' ' || cp == U'\t' || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A && cp != 0x2007) ||
           cp == 0x205F || cp == 0x3000;
}

inline void wrap_line(std::vector<std::string>& out, std::string_view line, std::size_t columns) {
    if (columns == 0 || line.empty()) {
        out.emplace_back(line);
        return;
    }

    // Track the most recent separator run that follows text. When a word
    // doesn't fit, move the entire word to the next line if possible.
    constexpr auto none = std::string_view::npos;
    std::size_t start = 0;
    while (start < line.size()) {
        std::size_t end = start;
        std::size_t width = 0;
        std::size_t separator = none;
        std::size_t after_separator = none;
        bool seen_text = false;
        bool in_separator = false;
        while (end < line.size()) {
            const auto next = grapheme::next_break(line, end);
            const auto cluster = line.substr(end, next - end);
            const auto added = static_cast<std::size_t>(charwidth::of_grapheme(cluster));
            const auto space = word_space(cluster);
            if (space && seen_text) {
                if (!in_separator) separator = end;
                after_separator = next;
            }
            if (added > columns - width && end != start) {
                if (separator != none) {
                    while (space && after_separator < line.size()) {
                        const auto space_end = grapheme::next_break(line, after_separator);
                        if (!word_space(line.substr(after_separator, space_end - after_separator))) break;
                        after_separator = space_end;
                    }
                    out.emplace_back(line.substr(start, separator - start));
                    start = after_separator;
                } else {
                    out.emplace_back(line.substr(start, end - start));
                    start = end;
                }
                break;
            }
            // A wide grapheme in a one-cell viewport still makes progress.
            width = std::min(columns, width + added);
            end = next;
            seen_text = seen_text || !space;
            in_separator = space;
        }
        if (end == line.size()) {
            out.emplace_back(line.substr(start));
            break;
        }
    }
}

}  // namespace detail

/** Measure the widest hard line in terminal cells; tabs are not expanded. */
[[nodiscard]] inline auto measure(std::string_view text) -> int {
    int widest = 0;
    int width = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = grapheme::next_break(text, start);
        const auto cluster = text.substr(start, end - start);
        if (detail::hard_break(cluster)) {
            widest = std::max(widest, width);
            width = 0;
        } else {
            const auto added = charwidth::of_grapheme(cluster);
            width += std::min(added, std::numeric_limits<int>::max() - width);
        }
        start = end;
    }
    return std::max(widest, width);
}

/**
 * Wrap at breakable spaces and tabs or, for oversized words, at grapheme boundaries.
 * Hard line breaks are preserved as line boundaries. Soft separator runs
 * are dropped at a wrap. Zero columns disables soft wrapping.
 */
[[nodiscard]] inline auto wrap(std::string_view text, std::size_t columns) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t line = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = grapheme::next_break(text, start);
        if (detail::hard_break(text.substr(start, end - start))) {
            detail::wrap_line(out, text.substr(line, start - line), columns);
            line = end;
        }
        start = end;
    }
    detail::wrap_line(out, text.substr(line), columns);
    return out;
}

/** Truncate to one line, reserving one cell for an ellipsis if text is omitted. */
[[nodiscard]] inline auto truncate(std::string_view text, std::size_t columns) -> std::string {
    if (columns == 0) return {};
    std::size_t prefix = 0;
    std::size_t width = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = grapheme::next_break(text, start);
        const auto cluster = text.substr(start, end - start);
        const auto added = static_cast<std::size_t>(charwidth::of_grapheme(cluster));
        if (detail::hard_break(cluster) || added > columns - width)
            return std::string{text.substr(0, prefix)} + "\xE2\x80\xA6";
        width += added;
        if (width < columns) prefix = end;
        start = end;
    }
    return std::string{text};
}

/** Rewrap the original logical text for a new terminal width. */
[[nodiscard]] inline auto reflow(std::string_view text, std::size_t columns) -> std::vector<std::string> {
    return wrap(text, columns);
}

}  // namespace ttytk::layout
