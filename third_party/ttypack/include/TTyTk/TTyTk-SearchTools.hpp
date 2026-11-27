#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>
#include <TTyTk/TTyTk-Scrollback.hpp>

#include <cstdint>
#include <regex>
#include <string_view>

namespace ttytk::search {
/** A line index, starting terminal column, and length in terminal columns. */
struct Match { std::size_t row{}, column{}, length{}; };
using Regex = std::regex;

namespace detail {
inline auto rows(const CellBuffer& source) -> std::size_t { return source.rows(); }
inline auto rows(const Scrollback& source) -> std::size_t { return source.size(); }
inline auto columns(const CellBuffer& source, std::size_t) -> std::size_t { return source.columns(); }
inline auto columns(const Scrollback& source, const std::size_t row) -> std::size_t {
    return source.line(row).size();
}
inline auto cell(const CellBuffer& source, const std::size_t row, const std::size_t col) -> const Cell& {
    return source.at(row, col);
}
inline auto cell(const Scrollback& source, const std::size_t row, const std::size_t col) -> const Cell& {
    return source.line(row)[col];
}

inline auto fold_ascii(const char c) -> char {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

template <typename Source, typename Scan>
inline auto collect(const Source& source, Scan scan, const bool first_only) -> std::vector<Match> {
    std::vector<Match> matches;
    for (std::size_t row = 0; row < rows(source); ++row) {
        std::string text;
        std::vector<std::size_t> byte_columns, byte_ends;
        const auto width = columns(source, row);
        for (std::size_t col = 0; col < width; ++col) {
            const auto& entry = cell(source, row, col);
            if (entry.continuation) continue;
            auto end = col + 1;
            while (end < width && cell(source, row, end).continuation) ++end;
            text += entry.grapheme;
            byte_columns.insert(byte_columns.end(), entry.grapheme.size(), col);
            byte_ends.insert(byte_ends.end(), entry.grapheme.size(), end);
        }
        scan(text, [&](const std::size_t offset, const std::size_t length) {
            const auto col = offset == byte_columns.size() ? width : byte_columns[offset];
            matches.push_back({row, col, length == 0 ? 0 : byte_ends[offset + length - 1] - col});
            return first_only;
        });
        if (first_only && !matches.empty()) break;
    }
    return matches;
}

inline auto literal(std::string needle, const bool ignore_case) {
    if (ignore_case) for (auto& c : needle) c = fold_ascii(c);
    return [needle = std::move(needle), ignore_case](std::string& text, auto emit) {
        if (needle.empty()) return;
        if (ignore_case) for (auto& c : text) c = fold_ascii(c);
        for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
            if (emit(at, needle.size())) break;
    };
}

inline auto regexp(const Regex& pattern) {
    return [&pattern](const std::string& text, auto emit) {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
             it != std::sregex_iterator{}; ++it) {
            if (emit(static_cast<std::size_t>(it->position()), static_cast<std::size_t>(it->length())))
                break;
        }
    };
}

template <typename Source>
concept Searchable = std::is_same_v<Source, CellBuffer> || std::is_same_v<Source, Scrollback>;
}  // namespace detail

/** Finds literal occurrences within each row; optional folding is ASCII-only. */
template <detail::Searchable Source>
[[nodiscard]] inline auto find_all(const Source& source, const std::string_view needle,
                                   const bool ignore_case = false) -> std::vector<Match> {
    return detail::collect(source, detail::literal(std::string{needle}, ignore_case), false);
}

template <detail::Searchable Source>
[[nodiscard]] inline auto find(const Source& source, const std::string_view needle,
                               const bool ignore_case = false) -> std::optional<Match> {
    auto matches = detail::collect(source, detail::literal(std::string{needle}, ignore_case), true);
    return matches.empty() ? std::nullopt : std::optional{matches.front()};
}

/** Finds matches of a compiled regex within each row. */
template <detail::Searchable Source>
[[nodiscard]] inline auto find_all(const Source& source, const Regex& pattern) -> std::vector<Match> {
    return detail::collect(source, detail::regexp(pattern), false);
}

template <detail::Searchable Source>
[[nodiscard]] inline auto find(const Source& source, const Regex& pattern) -> std::optional<Match> {
    auto matches = detail::collect(source, detail::regexp(pattern), true);
    return matches.empty() ? std::nullopt : std::optional{matches.front()};
}

/** Finds cells containing every attribute bit in mask, including both columns of wide glyphs. */
template <detail::Searchable Source>
[[nodiscard]] inline auto find_all(const Source& source, const std::uint32_t mask) -> std::vector<Match> {
    std::vector<Match> matches;
    for (std::size_t row = 0; row < detail::rows(source); ++row) {
        const auto width = detail::columns(source, row);
        for (std::size_t col = 0; col < width; ++col) {
            if ((detail::cell(source, row, col).attributes & mask) == mask) {
                if (!matches.empty() && matches.back().row == row &&
                    matches.back().column + matches.back().length == col) ++matches.back().length;
                else matches.push_back({row, col, 1});
            }
        }
    }
    return matches;
}
}  // namespace ttytk::search
