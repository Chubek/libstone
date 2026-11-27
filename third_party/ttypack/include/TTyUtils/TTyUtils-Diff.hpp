#pragma once

#include <TTyUtils/TTyUtils-TextBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ttyutils::diff {

struct Hunk {
    enum class Kind { equal, add, remove };
    Kind kind{Kind::equal};
    std::size_t old_line{};
    std::size_t new_line{};
    std::vector<std::string> content{};
    std::vector<bool> terminated{};  ///< Whether the corresponding line had an LF.
};

namespace detail {

using Token = std::string_view;
using Kind = Hunk::Kind;

[[nodiscard]] inline auto split_lines(const std::string_view text) -> std::vector<Token> {
    std::vector<Token> result;
    for (std::size_t start = 0; start < text.size();) {
        const auto lf = text.find('\n', start);
        const auto end = lf == Token::npos ? text.size() : lf + 1;
        result.push_back(text.substr(start, end - start));
        start = end;
    }
    return result;
}

/** Trace Myers' shortest edit path. Bound memory and fall back on sparse anchors. */
[[nodiscard]] inline auto myers(const std::vector<Token>& old, const std::vector<Token>& updated,
                                  std::vector<Kind>& script) -> bool {
    using Index = std::ptrdiff_t;
    const auto n = static_cast<Index>(old.size());
    const auto m = static_cast<Index>(updated.size());
    if (n == 0 || m == 0) {
        script.insert(script.end(), old.size(), Kind::remove);
        script.insert(script.end(), updated.size(), Kind::add);
        return true;
    }
    std::vector<std::vector<Index>> trace;
    const auto diagonal = [&](Index d, Index k) { return trace[d][(k + d) / 2]; };
    const auto down = [&](Index d, Index k) {
        return k == -d || (k != d && diagonal(d - 1, k - 1) < diagonal(d - 1, k + 1));
    };
    for (Index d = 0; d <= n + m && d <= 512; ++d) {
        trace.emplace_back(static_cast<std::size_t>(d + 1));
        for (Index k = -d; k <= d; k += 2) {
            auto x = d == 0 ? Index{0} : down(d, k) ? diagonal(d - 1, k + 1) : diagonal(d - 1, k - 1) + 1;
            auto y = x - k;
            while (x < n && y < m && old[x] == updated[y]) { ++x; ++y; }
            trace[d][(k + d) / 2] = x;
            if (x != n || y != m) continue;

            const auto start = script.size();
            while (d > 0) {
                k = x - y;
                const auto added = down(d, k);
                const auto previous_k = k + (added ? 1 : -1);
                const auto previous_x = diagonal(d - 1, previous_k);
                const auto previous_y = previous_x - previous_k;
                while (x > previous_x && y > previous_y) { script.push_back(Kind::equal); --x; --y; }
                script.push_back(added ? Kind::add : Kind::remove);
                x = previous_x;
                y = previous_y;
                --d;
            }
            script.insert(script.end(), static_cast<std::size_t>(x), Kind::equal);
            std::reverse(script.begin() + static_cast<std::ptrdiff_t>(start), script.end());
            return true;
        }
    }
    return false;
}

inline void anchored(const std::vector<Token>& old, const std::vector<Token>& updated,
                     std::vector<Kind>& script) {
    struct Occurrence { std::size_t count{}, index{}; };
    std::unordered_map<Token, Occurrence> old_histogram, new_histogram;
    for (std::size_t i = 0; i < old.size(); ++i) {
        auto& entry = old_histogram[old[i]];
        ++entry.count;
        entry.index = i;
    }
    for (std::size_t i = 0; i < updated.size(); ++i) {
        auto& entry = new_histogram[updated[i]];
        ++entry.count;
        entry.index = i;
    }
    constexpr auto none = std::numeric_limits<std::size_t>::max();
    struct Anchor { std::size_t old_at, new_at, prior; };
    std::vector<Anchor> anchors;
    std::vector<std::size_t> tails;
    for (std::size_t i = 0; i < old.size(); ++i) {
        if (old_histogram.at(old[i]).count != 1) continue;
        const auto found = new_histogram.find(old[i]);
        if (found == new_histogram.end() || found->second.count != 1) continue;
        const auto j = found->second.index;
        const auto tail = std::lower_bound(tails.begin(), tails.end(), j,
            [&](auto index, auto target) { return anchors[index].new_at < target; });
        const auto node = anchors.size();
        anchors.push_back({i, j, tail == tails.begin() ? none : *(tail - 1)});
        if (tail == tails.end()) tails.push_back(node);
        else *tail = node;
    }
    std::vector<Anchor> path;
    for (auto node = tails.empty() ? none : tails.back(); node != none; node = anchors[node].prior)
        path.push_back(anchors[node]);
    std::reverse(path.begin(), path.end());
    path.push_back({old.size(), updated.size(), none});
    std::size_t i = 0, j = 0;
    for (const auto& anchor : path) {
        // Fast common edges in each gap also keep repeated context.
        while (i < anchor.old_at && j < anchor.new_at && old[i] == updated[j]) {
            script.push_back(Kind::equal); ++i; ++j;
        }
        std::size_t suffix = 0;
        while (suffix < anchor.old_at - i && suffix < anchor.new_at - j &&
               old[anchor.old_at - suffix - 1] == updated[anchor.new_at - suffix - 1]) ++suffix;
        script.insert(script.end(), anchor.old_at - i - suffix, Kind::remove);
        script.insert(script.end(), anchor.new_at - j - suffix, Kind::add);
        script.insert(script.end(), suffix, Kind::equal);
        if (anchor.old_at != old.size()) script.push_back(Kind::equal);
        i = anchor.old_at + 1;
        j = anchor.new_at + 1;
    }
}

[[nodiscard]] inline auto tokens(const std::vector<Token>& old, const std::vector<Token>& updated,
                                 bool by_line) -> std::vector<Hunk> {
    std::vector<Kind> script;
    if (!myers(old, updated, script)) anchored(old, updated, script);
    std::vector<Hunk> result;
    std::size_t i = 0, j = 0;
    for (const auto kind : script) {
        if (result.empty() || result.back().kind != kind) result.push_back({kind, i, j});
        auto text = kind == Kind::add ? updated[j] : old[i];
        const bool terminated = by_line && text.ends_with('\n');
        if (terminated) text.remove_suffix(1);
        result.back().content.emplace_back(text);
        result.back().terminated.push_back(terminated);
        i += kind != Kind::add;
        j += kind != Kind::remove;
    }
    return result;
}

[[nodiscard]] inline auto split_words(Token text) -> std::vector<Token> {
    const auto classify = [](unsigned char c) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return 0;
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') return 1;
        return 2;
    };
    std::vector<Token> result;
    for (std::size_t start = 0; start < text.size();) {
        const auto type = classify(text[start]);
        auto end = start + 1;
        while (type != 2 && end < text.size() && classify(text[end]) == type) ++end;
        result.push_back(text.substr(start, end - start));
        start = end;
    }
    return result;
}

[[nodiscard]] inline auto terminated(const Hunk& hunk, std::size_t i) -> bool {
    return i >= hunk.terminated.size() || hunk.terminated[i];
}

inline void append(std::string& output, const Hunk& hunk, std::size_t i) {
    output += hunk.content[i];
    if (terminated(hunk, i)) output += '\n';
}

}  // namespace detail

/** Diff file lines, treating a missing final newline as significant. */
[[nodiscard]] inline auto lines(const textbuf::Snapshot& old_snapshot,
                                const textbuf::Snapshot& new_snapshot) -> std::vector<Hunk> {
    return detail::tokens(detail::split_lines(old_snapshot.text()), detail::split_lines(new_snapshot.text()), true);
}

[[nodiscard]] inline auto words(const std::string_view old_text, const std::string_view new_text) -> std::vector<Hunk> {
    return detail::tokens(detail::split_words(old_text), detail::split_words(new_text), false);
}

/** In-memory patch with unified hunk serialization and checked application. */
class Patch {
public:
    Patch() = default;
    explicit Patch(std::vector<Hunk> hunks) : hunks_(std::move(hunks)) {}

    [[nodiscard]] static auto unified(const std::vector<Hunk>& hunks, const std::size_t context = 3) -> Patch {
        Patch patch{hunks};
        patch.context_ = context;
        return patch;
    }
    [[nodiscard]] auto hunks() const noexcept -> const std::vector<Hunk>& { return hunks_; }

    [[nodiscard]] auto str() const -> std::string {
        struct Line { const Hunk* hunk; std::size_t index, old_at, new_at; };
        std::vector<Line> all;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (const auto& hunk : hunks_) {
            for (std::size_t i = 0; i < hunk.content.size(); ++i)
                all.push_back({&hunk, i, hunk.old_line + (hunk.kind == detail::Kind::add ? 0 : i),
                    hunk.new_line + (hunk.kind == detail::Kind::remove ? 0 : i)});
        }
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].hunk->kind == detail::Kind::equal) continue;
            const auto first = i - std::min(i, context_);
            const auto last = i + 1 + std::min(all.size() - i - 1, context_);
            if (!ranges.empty() && first <= ranges.back().second) ranges.back().second = last;
            else ranges.emplace_back(first, last);
        }
        std::string output;
        for (const auto& [first, last] : ranges) {
            std::size_t old_count = 0, new_count = 0;
            for (auto at = first; at < last; ++at) {
                old_count += all[at].hunk->kind != detail::Kind::add;
                new_count += all[at].hunk->kind != detail::Kind::remove;
            }
            const auto range = [](std::size_t start, std::size_t count) {
                auto text = std::to_string(start + (count != 0));
                if (count != 1) text += "," + std::to_string(count);
                return text;
            };
            output += "@@ -" + range(all[first].old_at, old_count) + " +" +
                      range(all[first].new_at, new_count) + " @@\n";
            for (auto at = first; at < last; ++at) {
                const auto& line = all[at];
                output += line.hunk->kind == detail::Kind::add ? '+' : line.hunk->kind == detail::Kind::remove ? '-' : ' ';
                output += line.hunk->content[line.index];
                output += '\n';
                if (!detail::terminated(*line.hunk, line.index)) output += "\\ No newline at end of file\n";
            }
        }
        return output;
    }

    /** Apply atomically, returning false if original lines do not match. */
    auto apply(textbuf::Buffer& buffer) const -> bool {
        const auto base = detail::split_lines(buffer.text());
        std::string output;
        std::size_t at = 0;
        for (const auto& hunk : hunks_) {
            if (hunk.old_line < at || hunk.old_line > base.size()) return false;
            while (at < hunk.old_line) output += base[at++];
            for (std::size_t i = 0; i < hunk.content.size(); ++i) {
                if (hunk.kind != detail::Kind::add) {
                    if (at == base.size()) return false;
                    const auto actual = base[at++];
                    const auto lf = actual.ends_with('\n');
                    if (lf != detail::terminated(hunk, i) || actual.substr(0, actual.size() - lf) != hunk.content[i])
                        return false;
                }
                if (hunk.kind != detail::Kind::remove) detail::append(output, hunk, i);
            }
        }
        while (at < base.size()) output += base[at++];
        buffer.replace_bytes(0, buffer.size(), output);
        return true;
    }

private:
    std::vector<Hunk> hunks_;
    std::size_t context_{3};
};

struct Conflict {
    std::string base{};
    std::string ours{};
    std::string theirs{};
};

struct MergeResult {
    textbuf::Buffer result{};
    std::vector<Conflict> conflicts{};
};

namespace detail {
struct Change {
    std::size_t first, last;
    std::string replacement;
};

[[nodiscard]] inline auto changes(const std::vector<Hunk>& hunks) -> std::vector<Change> {
    std::vector<Change> result;
    bool open = false;
    for (const auto& hunk : hunks) {
        if (hunk.kind == Kind::equal) { open = false; continue; }
        if (!open) {
            result.push_back({hunk.old_line, hunk.old_line, {}});
            open = true;
        }
        if (hunk.kind == Kind::remove) result.back().last += hunk.content.size();
        else for (std::size_t i = 0; i < hunk.content.size(); ++i) append(result.back().replacement, hunk, i);
    }
    return result;
}

[[nodiscard]] inline auto overlaps(const Change& a, const Change& b) -> bool {
    if (a.first == b.first) return true;
    return a.first < b.last && b.first < a.last;
}
}  // namespace detail

/** Merge disjoint edits; report localized conflicts for overlapping alternatives. */
[[nodiscard]] inline auto merge3(const textbuf::Snapshot& base, const textbuf::Snapshot& ours,
                                 const textbuf::Snapshot& theirs) -> MergeResult {
    if (ours.text() == theirs.text()) return {textbuf::Buffer{std::string(ours.text())}, {}};
    if (ours.text() == base.text()) return {textbuf::Buffer{std::string(theirs.text())}, {}};
    if (theirs.text() == base.text()) return {textbuf::Buffer{std::string(ours.text())}, {}};
    const auto original = detail::split_lines(base.text());
    const auto left = detail::changes(lines(base, ours));
    const auto right = detail::changes(lines(base, theirs));
    const auto append_base = [&](std::string& output, std::size_t first, std::size_t last) {
        for (; first < last; ++first) output += original[first];
    };
    const auto render = [&](const auto& changes, std::size_t from, std::size_t to, std::size_t first, std::size_t last) {
        std::string output;
        for (; from < to; ++from) {
            append_base(output, first, changes[from].first);
            output += changes[from].replacement;
            first = changes[from].last;
        }
        append_base(output, first, last);
        return output;
    };
    std::string output;
    std::vector<Conflict> conflicts;
    for (std::size_t i = 0, j = 0, at = 0;;) {
        if (i == left.size() && j == right.size()) { append_base(output, at, original.size()); break; }
        if (i < left.size() && j < right.size() && detail::overlaps(left[i], right[j])) {
            const auto first = std::min(left[i].first, right[j].first);
            auto last = std::max(left[i].last, right[j].last);
            auto a = i + 1, b = j + 1;
            bool grew = true;
            while (grew) {
                grew = false;
                while (a < left.size() && left[a].first < last) { last = std::max(last, left[a++].last); grew = true; }
                while (b < right.size() && right[b].first < last) { last = std::max(last, right[b++].last); grew = true; }
            }
            append_base(output, at, first);
            auto one = render(left, i, a, first, last), two = render(right, j, b, first, last);
            if (one == two) output += one;
            else {
                std::string ancestor;
                append_base(ancestor, first, last);
                const auto section = [&output](std::string_view text) {
                    output += text;
                    if (!text.empty() && !text.ends_with('\n')) output += '\n';
                };
                if (!output.empty() && !output.ends_with('\n')) output += '\n';
                output += "<<<<<<< ours\n";
                section(one);
                output += "=======\n";
                section(two);
                output += ">>>>>>> theirs\n";
                conflicts.push_back({std::move(ancestor), std::move(one), std::move(two)});
            }
            i = a; j = b; at = last;
        } else {
            const auto& change = j == right.size() || (i < left.size() && left[i].first < right[j].first)
                ? left[i++] : right[j++];
            append_base(output, at, change.first);
            output += change.replacement;
            at = change.last;
        }
    }
    return {textbuf::Buffer{std::move(output)}, std::move(conflicts)};
}

}  // namespace ttyutils::diff
