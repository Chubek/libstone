#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ttytk::prompt {

struct Segment {
    std::string text{};
    color::Rgb foreground{255, 255, 255};
    color::Rgb background{};
    std::string icon{};
    std::function<std::string()> evaluate{};
};

using SegmentList = std::vector<Segment>;

/** Renders the prefix of complete segments that fits in the terminal width.
 *  A zero limit means unlimited. A segment callback, if set, replaces text.
 */
[[nodiscard]] inline auto render(const SegmentList& segments, const std::size_t columns = 0)
    -> std::string {
    std::string output;
    std::size_t used = 0;
    const auto style = [](const color::Rgb fg, const color::Rgb bg) {
        const auto rgb = [](const color::Rgb value) {
            return std::to_string(value.red) + ';' + std::to_string(value.green) + ';' +
                   std::to_string(value.blue);
        };
        return "\x1b[38;2;" + rgb(fg) + ";48;2;" + rgb(bg) + 'm';
    };
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const auto& segment = segments[index];
        const auto content = segment.icon + (segment.evaluate ? segment.evaluate() : segment.text);
        std::size_t visible = index != 0 ? 1U : 0U;
        for (std::size_t start = 0; start < content.size();) {
            const auto end = grapheme::next_break(content, start);
            visible += static_cast<std::size_t>(
                charwidth::of_grapheme(std::string_view{content}.substr(start, end - start)));
            start = end;
        }
        if (columns != 0 && visible > columns - used) break;
        if (index != 0) output += style(segments[index - 1].background, segment.background) + "\xee\x82\xb0";
        output += style(segment.foreground, segment.background) + content;
        used += visible;
    }
    if (!output.empty()) output += "\x1b[0m";
    return output;
}

}  // namespace ttytk::prompt
