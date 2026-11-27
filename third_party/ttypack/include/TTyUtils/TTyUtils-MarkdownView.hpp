#pragma once

#include <TTyTk/TTyTk-Encoding.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyUtils/TTyUtils-Pager.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::mdview {

struct Options {
    bool links{true};
    bool code_highlighting{true};
    std::size_t width{80};
};

struct TocEntry {
    std::string title{};
    std::size_t line{};
    int level{};
};

namespace detail {

struct Document {
    std::string text;
    std::vector<TocEntry> toc;
};

[[nodiscard]] inline auto visible(const std::string_view text, const bool links) -> std::string {
    std::string value;
    for (const auto cp : ttytk::utf8::decode(text)) {
        if (cp == U'\t') value += ' ';
        else if (cp >= 0x20 && (cp < 0x7f || cp >= 0xa0)) value += ttytk::utf8::encode(cp);
    }
    if (!links) return value;
    std::string result;
    std::string_view rest{value};
    while (!rest.empty()) {
        const auto open = rest.find('[');
        if (open == std::string_view::npos) break;
        const auto target = rest.find("](", open + 1);
        if (target == std::string_view::npos) break;
        const auto close = rest.find(')', target + 2);
        if (close == std::string_view::npos) break;
        result += rest.substr(0, open);
        result += rest.substr(open + 1, target - open - 1);
        rest.remove_prefix(close + 1);
    }
    return result + std::string(rest);
}

[[nodiscard]] inline auto project(const std::string_view markdown, const Options& options, const bool styled)
    -> Document {
    Document doc;
    std::size_t row = 0;
    char fence = 0;
    std::size_t fence_width = 0;
    for (std::size_t start = 0; start < markdown.size();) {
        const auto end = markdown.find('\n', start);
        auto line = markdown.substr(start, end == std::string_view::npos ? end : end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto indent = line.find_first_not_of(' ');
        const auto block = indent <= 3 ? line.substr(indent) : std::string_view{};
        if (block.starts_with("```") || block.starts_with("~~~")) {
            const auto ahead = block.find_first_not_of(block.front());
            const auto count = ahead == std::string_view::npos ? block.size() : ahead;
            if (!fence || (fence == block.front() && count >= fence_width &&
                           block.substr(count).find_first_not_of(" \t") == std::string_view::npos)) {
                fence = fence ? 0 : block.front();
                fence_width = count;
                doc.text += '\n'; ++row;
                if (end == std::string_view::npos) break;
                start = end + 1;
                continue;
            }
        }
        std::size_t level = 0;
        if (!fence && block.starts_with('#')) {
            const auto after = block.find_first_not_of('#');
            const auto count = after == std::string_view::npos ? block.size() : after;
            if (count <= 6 && (count == block.size() || block[count] == ' ' || block[count] == '\t')) {
                level = count;
                const auto first = block.find_first_not_of(" \t", count);
                line = first == std::string_view::npos ? std::string_view{} : block.substr(first);
            }
        }
        const auto value = visible(line, !fence && options.links);
        if (level) doc.toc.push_back({value, row, static_cast<int>(level)});
        const int width = static_cast<int>(std::min(options.width,
                                                    static_cast<std::size_t>(std::numeric_limits<int>::max())));
        for (auto rest = std::string_view(value);;) {
            auto part = width > 0 && !fence ? ui::fit_text(rest, width) : rest;
            if (part.empty() && !rest.empty()) part = rest.substr(0, ttytk::grapheme::next_break(rest, 0));
            if (styled && level) doc.text += "\x1b[1m";
            else if (styled && fence && options.code_highlighting) doc.text += "\x1b[2m";
            doc.text += part;
            if (styled && (level || (fence && options.code_highlighting))) doc.text += "\x1b[0m";
            doc.text += '\n'; ++row;
            rest.remove_prefix(part.size());
            if (rest.empty()) break;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return doc;
}

}  // namespace detail

/// Render a plain-text subset: ATX headings, fenced code and inline link labels.
/// Literal terminal controls are discarded; with link labels enabled their
/// destinations are not emitted.
inline auto render(const std::string_view markdown, const Options& options = {}) -> std::string {
    return detail::project(markdown, options, false).text;
}

class MarkdownView : public pager::Pager {
public:
    MarkdownView() { set_wrap(true); }
    void set_text(std::string text) { text_ = std::move(text); rebuild(); }
    void set_options(const Options& options) { options_ = options; rebuild(); }
    void jump(const TocEntry& heading) { scroll_to(heading.line); }
    [[nodiscard]] auto toc() const noexcept -> const std::vector<TocEntry>& { return toc_; }

private:
    void rebuild() {
        auto doc = detail::project(text_, options_, true);
        toc_ = std::move(doc.toc);
        set_source(pager::Source{std::move(doc.text)});
    }
    std::string text_;
    Options options_;
    std::vector<TocEntry> toc_;
};

}  // namespace ttyutils::mdview
