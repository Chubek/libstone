#pragma once

#include <TTyTk/TTyTk-CharWidth.hpp>
#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyUtils/TTyUtils-TextBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::wrap {

class BreakRules {
public:
    constexpr BreakRules(bool break_words = true, bool preserve_code_tokens = false,
                         std::size_t hanging_indent = 0) noexcept
        : break_words_(break_words), preserve_code_tokens_(preserve_code_tokens), hanging_indent_(hanging_indent) {}

    [[nodiscard]] static constexpr auto prose() noexcept -> BreakRules { return {}; }
    [[nodiscard]] static constexpr auto code() noexcept -> BreakRules { return {true, true}; }
    [[nodiscard]] constexpr auto break_words() const noexcept -> bool { return break_words_; }
    [[nodiscard]] constexpr auto preserve_code_tokens() const noexcept -> bool { return preserve_code_tokens_; }
    [[nodiscard]] constexpr auto hanging_indent() const noexcept -> std::size_t { return hanging_indent_; }

private:
    bool break_words_{};
    bool preserve_code_tokens_{};
    std::size_t hanging_indent_{};
};

class WrapMap {
public:
    struct VisualPosition {
        std::size_t row{};
        std::size_t col{};
    };

    struct Row {
        textbuf::Buffer::Position start{};
        textbuf::Buffer::Position end{};
    };

    [[nodiscard]] auto rows() const noexcept -> const std::vector<Row>& { return rows_; }

    /** Visual columns are terminal cells; the interior of a wide cell maps to its grapheme. */
    [[nodiscard]] auto visual_to_buffer(const VisualPosition position) const -> textbuf::Buffer::Position {
        if (rows_.empty()) return {};
        const auto at = std::min(position.row, rows_.size() - 1);
        const auto& row = rows_[at];
        const auto& stops = cells_[at];
        const auto found = std::upper_bound(stops.begin(), stops.end(), position.col);
        return {row.start.line, row.start.col + (found == stops.begin() ? 0 :
            static_cast<std::size_t>(found - stops.begin() - 1))};
    }

    [[nodiscard]] auto buffer_to_visual(const textbuf::Buffer::Position position) const -> VisualPosition {
        const auto after = std::upper_bound(rows_.begin(), rows_.end(), position,
            [](auto pos, const Row& row) {
                return pos.line < row.start.line || (pos.line == row.start.line && pos.col < row.start.col);
            });
        if (after == rows_.begin()) return {};
        const auto row = static_cast<std::size_t>(after - rows_.begin() - 1);
        const auto col = position.line == rows_[row].start.line ?
            std::min(position.col, rows_[row].end.col) - rows_[row].start.col : cells_[row].size() - 1;
        return {row, cells_[row][col]};
    }

private:
    friend auto soft_wrap(const textbuf::Snapshot&, std::size_t, BreakRules) -> WrapMap;
    std::vector<Row> rows_;
    std::vector<std::vector<std::size_t>> cells_;
};

namespace detail {

[[nodiscard]] inline auto cells(std::string_view cluster, std::size_t column) -> std::size_t {
    if (cluster.starts_with('\t')) return 8 - column % 8;
    return static_cast<std::size_t>(ttytk::charwidth::of_grapheme(cluster));
}

[[nodiscard]] inline auto opportunity(char32_t left, char32_t right, BreakRules rules) -> bool {
    if (left == 0xA0 || right == 0xA0 || left == 0x2060 || right == 0x2060) return false;
    if (left == U' ' || left == U'\t' || left == 0x200B) return true;
    if (right == U' ' || right == U'\t' || rules.preserve_code_tokens()) return false;
    if (left == U'-' || left == 0x2010 || left == U'/') return true;
    const auto ideograph = [](char32_t c) {
        return (c >= 0x3040 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) ||
               (c >= 0x20000 && c <= 0x323AF);
    };
    constexpr std::u32string_view opening = U"([{（［｛「『《〈【";
    constexpr std::u32string_view closing = U")]},.;:!?）］｝」』》〉】、。！？";
    return (ideograph(left) || ideograph(right)) && opening.find(left) == std::u32string_view::npos &&
           closing.find(right) == std::u32string_view::npos;
}

}  // namespace detail

[[nodiscard]] inline auto soft_wrap(const textbuf::Snapshot& snapshot, const std::size_t width,
                                    const BreakRules rules = BreakRules::prose()) -> WrapMap {
    WrapMap map;
    const auto index = snapshot.line_index();
    const auto limit = width == 0 ? 1 : width;
    for (std::size_t line = 0; line < index.lines(); ++line) {
        const auto text = snapshot.line(line);
        if (text.empty()) {
            map.rows_.push_back({{line, 0}, {line, 0}});
            map.cells_.push_back({0});
            continue;
        }
        const auto clusters = ttytk::grapheme::segment(text);
        const auto cp = [&](std::size_t at) {
            return ttytk::detail::decode_utf8_scalar(clusters[at], 0).value;
        };
        std::size_t start = 0;
        while (start < clusters.size()) {
            std::size_t end = start;
            std::size_t last_break = start;
            std::vector<std::size_t> stops{start == 0 ? 0 : std::min(rules.hanging_indent(), limit - 1)};
            while (end < clusters.size()) {
                if (end > start && detail::opportunity(cp(end - 1), cp(end), rules)) last_break = end;
                const auto next = stops.back() + detail::cells(clusters[end], stops.back());
                if (next > limit && end > start) {
                    if (last_break != start) {
                        end = last_break;
                        stops.resize(end - start + 1);
                        break;
                    }
                    if (rules.break_words() && !rules.preserve_code_tokens() &&
                        cp(end - 1) != 0xA0 && cp(end) != 0xA0 &&
                        cp(end - 1) != 0x2060 && cp(end) != 0x2060) break;
                }
                stops.push_back(next);
                ++end;
            }
            map.rows_.push_back({{line, start}, {line, end}});
            map.cells_.push_back(std::move(stops));
            start = end;
        }
    }
    return map;
}

/** Reflow prose within a logical range; leave surrounding bytes untouched. */
inline void reflow(textbuf::Buffer& buffer, const textbuf::LineIndex::Range range, const std::size_t width,
                   const BreakRules rules = BreakRules::prose()) {
    const auto index = buffer.line_index();
    const auto begin = index.offset(range.begin, buffer.text());
    const auto end = index.offset(range.end, buffer.text());
    if (end <= begin) return;
    const auto source = buffer.text().substr(begin, end - begin);
    const std::string_view eol = buffer.eol() == textbuf::Buffer::Eol::crlf ? "\r\n" : "\n";
    std::string result;
    std::string paragraph;
    auto flush = [&] {
        if (paragraph.empty()) return;
        const auto snapshot = textbuf::Buffer{paragraph}.snapshot();
        const auto map = soft_wrap(snapshot, width, rules);
        const auto row_index = snapshot.line_index();
        bool first = true;
        for (const auto& row : map.rows()) {
            const auto row_start = row_index.offset(row.start, paragraph);
            const auto row_end = row_index.offset(row.end, paragraph);
            auto line = std::string_view(paragraph).substr(row_start, row_end - row_start);
            const auto content = line.find_first_not_of(' ');
            if (content == std::string_view::npos) continue;
            line = line.substr(content, line.find_last_not_of(' ') - content + 1);
            if (!first) {
                result += eol;
                result.append(std::min(rules.hanging_indent(), width == 0 ? 0 : width - 1), ' ');
            }
            result += line;
            first = false;
        }
        paragraph.clear();
    };
    for (std::size_t at = 0; at < source.size();) {
        const auto lf = source.find('\n', at);
        const auto stop = lf == std::string_view::npos ? source.size() : lf;
        auto line = source.substr(at, stop - at);
        if (lf != std::string_view::npos && line.ends_with('\r')) line.remove_suffix(1);
        if (line.find_first_not_of(" \t") == std::string_view::npos) {
            if (!paragraph.empty()) { flush(); result += eol; }
            result.append(source.substr(at, stop - at + (lf != std::string_view::npos)));
        } else {
            for (auto word = line.find_first_not_of(" \t"); word != std::string_view::npos;) {
                const auto next = line.find_first_of(" \t", word);
                if (!paragraph.empty()) paragraph += ' ';
                paragraph += line.substr(word, next == std::string_view::npos ? next : next - word);
                word = next == std::string_view::npos ? next : line.find_first_not_of(" \t", next);
            }
        }
        at = stop + (lf != std::string_view::npos);
    }
    if (!paragraph.empty()) {
        flush();
        if (source.ends_with('\n')) result += eol;
    }
    buffer.replace_bytes(begin, end, result);
}

inline void reflow(textbuf::Buffer& buffer, const std::size_t width,
                   const BreakRules rules = BreakRules::prose()) {
    reflow(buffer, {{}, {buffer.line_index().lines(), 0}}, width, rules);
}

}  // namespace ttyutils::wrap
