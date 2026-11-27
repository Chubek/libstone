#pragma once

#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyUtils/TTyUtils-TextBuffer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ttyutils::search {

struct Match {
    textbuf::Buffer::Position begin{};
    textbuf::Buffer::Position end{};
    int score{};
    textbuf::LineIndex::Range range{};
    std::vector<std::optional<std::string>> captures{};  ///< Capture zero is the whole regex match.
};

class Query {
public:
    enum class Kind { literal, regex, fuzzy };

    [[nodiscard]] static auto literal(std::string value, const bool case_sensitive = true) -> Query {
        return {Kind::literal, std::move(value), case_sensitive};
    }
    [[nodiscard]] static auto regex(std::string value, const bool case_sensitive = true) -> Query {
        return {Kind::regex, std::move(value), case_sensitive};
    }
    [[nodiscard]] static auto fuzzy(std::string value, const bool case_sensitive = false) -> Query {
        return {Kind::fuzzy, std::move(value), case_sensitive};
    }

    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto text() const noexcept -> std::string_view { return text_; }
    [[nodiscard]] auto case_sensitive() const noexcept -> bool { return case_sensitive_; }

private:
    Query(Kind kind, std::string value, bool case_sensitive)
        : kind_(kind), text_(std::move(value)), case_sensitive_(case_sensitive) {}
    Kind kind_;
    std::string text_;
    bool case_sensitive_{};
};

namespace detail {

[[nodiscard]] constexpr auto fold(char32_t c) noexcept -> char32_t {
    if ((c >= U'A' && c <= U'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ||
        (c >= 0x391 && c <= 0x3AB && c != 0x3A2) || (c >= 0x410 && c <= 0x42F)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if ((c >= 0x100 && c <= 0x12F && c % 2 == 0) || (c >= 0x132 && c <= 0x137 && c % 2 == 0) ||
        (c >= 0x139 && c <= 0x148 && c % 2 != 0) || (c >= 0x14A && c <= 0x177 && c % 2 == 0) ||
        (c >= 0x179 && c <= 0x17E && c % 2 != 0)) return c + 1;
    switch (c) {
    case 0x178: return 0xFF;
    case 0x17F: return U's';
    case 0x386: return 0x3AC;
    case 0x388: return 0x3AD;
    case 0x389: return 0x3AE;
    case 0x38A: return 0x3AF;
    case 0x38C: return 0x3CC;
    case 0x38E: return 0x3CD;
    case 0x38F: return 0x3CE;
    case 0x3C2: return 0x3C3;
    case 0x1E9E: return 0xDF;
    case 0x212A: return U'k';
    case 0x212B: return 0xE5;
    default: return c;
    }
}

[[nodiscard]] constexpr auto word(char32_t c) noexcept -> bool {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9') || c == U'_' ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c != 0xFEFF &&
            !(c >= 0x2000 && c <= 0x2BFF) && !(c >= 0x3000 && c <= 0x303F) &&
            !(c >= 0x1F000 && c <= 0x1FAFF));
}

struct Input {
    std::vector<char32_t> values;
    std::vector<std::size_t> bytes;
    std::vector<bool> breaks;
    Input(std::string_view view, std::size_t start) {
        auto next_break = start;
        for (std::size_t at = start; at < view.size();) {
            const auto scalar = ttytk::detail::decode_utf8_scalar(view, at);
            bytes.push_back(at);
            breaks.push_back(at == next_break);
            if (at == next_break) next_break = ttytk::grapheme::next_break(view, at);
            values.push_back(scalar.value);
            at += scalar.length;
        }
        bytes.push_back(view.size());
        breaks.push_back(true);
    }
};

/** Ordered Thompson NFA: each state/input pair is evaluated at most once per start. */
class Regex {
    enum class Op { accept, jump, split, scalar, any, charclass, begin, end, boundary, save };
    struct State {
        Op op;
        int out{-1}, alternate{-1};
        char32_t value{};
        int index{};
    };
    struct CharClass {
        std::vector<std::pair<char32_t, char32_t>> ranges;
        std::u32string categories;
        bool negated{};
    };
    struct Edge { int at; bool alternate{}; };
    struct Fragment { int start; std::vector<Edge> ends; };
    struct Invalid {};

public:
    using Failures = std::unordered_set<std::size_t>;
    static constexpr auto no_capture = std::numeric_limits<std::size_t>::max();
    struct Result {
        std::size_t end;
        std::vector<std::size_t> captures;
    };

    explicit Regex(std::string_view pattern, bool sensitive)
        : pattern_(Input{pattern, 0}.values), sensitive_(sensitive) {
        if (pattern_.size() > 8192) return;
        try {
            auto expression = parse_alternation(0);
            if (!done()) return;
            patch(expression.ends, emit({Op::accept}));
            start_ = expression.start;
        } catch (const Invalid&) {}
    }

    [[nodiscard]] auto valid() const -> bool { return start_ >= 0; }

    /** A failure memo may be shared across attempts on the exact same Input. */
    [[nodiscard]] auto run(const Input& input, std::size_t at, Failures* failures = nullptr,
                           std::size_t* budget = nullptr) const
        -> std::optional<Result> {
        struct Thread { int state; std::size_t at; std::vector<std::size_t> captures; };
        if (!valid()) return std::nullopt;
        std::vector<Thread> pending;
        Failures visited;
        pending.push_back({start_, at, std::vector<std::size_t>(group_count_ * 2 + 2, no_capture)});
        while (!pending.empty()) {
            auto thread = std::move(pending.back());
            pending.pop_back();
            for (;;) {
                if (budget) {
                    if (*budget == 0) return std::nullopt;
                    --(*budget);
                }
                const auto key = thread.at * states_.size() + static_cast<std::size_t>(thread.state);
                if (failures && failures->contains(key)) break;
                if (!visited.insert(key).second) break;
                const auto& state = states_[thread.state];
                const auto pos = thread.at;
                const auto end = pos == input.values.size();
                const auto c = end ? U'\0' : input.values[pos];
                const auto prev = pos == 0 ? U'\0' : input.values[pos - 1];
                bool passed = true;
                switch (state.op) {
                case Op::accept:
                    if (input.breaks[pos]) return Result{pos, std::move(thread.captures)};
                    passed = false;
                    break;
                case Op::jump: break;
                case Op::split: pending.push_back({state.alternate, pos, thread.captures}); break;
                case Op::save: thread.captures[state.index] = pos; break;
                case Op::begin: passed = pos == 0; break;
                case Op::end: passed = end; break;
                case Op::boundary:
                    passed = (word(prev) != word(c)) != (state.value == U'B');
                    break;
                case Op::scalar:
                case Op::any:
                case Op::charclass:
                    passed = !end && (state.op == Op::scalar ? canonical(c) == canonical(state.value) :
                        state.op == Op::any ? c != U'\r' && c != U'\n' : character(classes_[state.index], c));
                    if (passed) ++thread.at;
                    break;
                }
                if (!passed) break;
                thread.state = state.out;
            }
        }
        if (failures) failures->merge(visited);
        return std::nullopt;
    }

private:
    [[nodiscard]] auto canonical(char32_t c) const -> char32_t { return sensitive_ ? c : fold(c); }
    [[nodiscard]] static auto category(char32_t type, char32_t c) -> bool {
        switch (type) {
        case U'd': return c >= U'0' && c <= U'9';
        case U'w': return word(c);
        case U's': return c == U' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 ||
                            (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x3000;
        default: return !category(type + 32, c);
        }
    }
    [[nodiscard]] auto character(const CharClass& rule, char32_t c) const -> bool {
        for (const auto type : rule.categories) if (category(type, c)) return !rule.negated;
        for (const auto& [first, last] : rule.ranges) {
            if ((first <= c && c <= last) ||
                (!sensitive_ && canonical(first) <= canonical(c) && canonical(c) <= canonical(last)))
                return !rule.negated;
        }
        return rule.negated;
    }
    [[nodiscard]] static auto is_category(char32_t c) -> bool {
        return std::u32string_view{U"dDwWsS"}.find(c) != std::u32string_view::npos;
    }
    [[nodiscard]] static auto escaped(char32_t c) -> char32_t {
        return c == U'n' ? U'\n' : c == U'r' ? U'\r' : c == U't' ? U'\t' : c;
    }
    [[nodiscard]] auto done() const -> bool { return cursor_ == pattern_.size(); }
    [[nodiscard]] auto at(char32_t value) const -> bool { return !done() && pattern_[cursor_] == value; }
    auto take() -> char32_t { if (done()) throw Invalid{}; return pattern_[cursor_++]; }
    auto emit(State state) -> int { states_.push_back(state); return static_cast<int>(states_.size() - 1); }
    auto atom(State state) -> Fragment {
        const auto index = emit(state);
        return {index, {{index}}};
    }
    void patch(const std::vector<Edge>& edges, int target) {
        for (const auto edge : edges) (edge.alternate ? states_[edge.at].alternate : states_[edge.at].out) = target;
    }
    auto concat(Fragment first, Fragment second) -> Fragment {
        patch(first.ends, second.start);
        return {first.start, std::move(second.ends)};
    }
    auto parse_alternation(unsigned depth) -> Fragment {
        auto left = parse_sequence(depth);
        while (at(U'|')) {
            take();
            auto right = parse_sequence(depth);
            left.start = emit({Op::split, left.start, right.start});
            left.ends.insert(left.ends.end(), right.ends.begin(), right.ends.end());
        }
        return left;
    }
    auto parse_sequence(unsigned depth) -> Fragment {
        auto result = atom({Op::jump});
        while (!done() && !at(U')') && !at(U'|')) {
            auto element = parse_atom(depth);
            if (at(U'*') || at(U'+') || at(U'?')) {
                const auto op = take();
                const bool lazy = at(U'?');
                if (lazy) take();
                const auto split = emit({Op::split, lazy ? -1 : element.start, lazy ? element.start : -1});
                if (op == U'?') element.ends.push_back({split, !lazy});
                else { patch(element.ends, split); element.ends = {{split, !lazy}}; }
                if (op != U'+') element.start = split;
            }
            result = concat(std::move(result), std::move(element));
        }
        return result;
    }
    auto parse_atom(unsigned depth) -> Fragment {
        const auto c = take();
        switch (c) {
        case U'.': return atom({Op::any});
        case U'^': return atom({Op::begin});
        case U'$': return atom({Op::end});
        case U'*': case U'+': case U'?': throw Invalid{};
        case U'(': {
            if (depth >= 64) throw Invalid{};
            const bool capturing = !at(U'?');
            if (!capturing) { take(); if (take() != U':') throw Invalid{}; }
            const auto group = capturing ? ++group_count_ : 0;
            if (group_count_ > 32) throw Invalid{};
            auto nested = parse_alternation(depth + 1);
            if (take() != U')') throw Invalid{};
            if (!capturing) return nested;
            auto start = atom({.op = Op::save, .index = group * 2});
            auto end = atom({.op = Op::save, .index = group * 2 + 1});
            return concat(concat(std::move(start), std::move(nested)), std::move(end));
        }
        case U'[': {
            CharClass value;
            if (at(U'^')) { take(); value.negated = true; }
            do {
                auto first = take();
                if (first == U'\\') {
                    first = take();
                    if (is_category(first)) { value.categories.push_back(first); continue; }
                    first = escaped(first);
                }
                auto last = first;
                if (at(U'-') && cursor_ + 1 < pattern_.size() && pattern_[cursor_ + 1] != U']') {
                    take();
                    last = take();
                    if (last == U'\\') last = escaped(take());
                    if (first > last) throw Invalid{};
                }
                value.ranges.emplace_back(first, last);
            } while (!at(U']'));
            take();
            return add_class(std::move(value));
        }
        case U'\\': {
            const auto value = take();
            if (value == U'b' || value == U'B') return atom({.op = Op::boundary, .value = value});
            if (is_category(value)) return add_class({{}, std::u32string(1, value), false});
            return atom({.op = Op::scalar, .value = escaped(value)});
        }
        default: return atom({.op = Op::scalar, .value = c});
        }
    }
    auto add_class(CharClass value) -> Fragment {
        classes_.push_back(std::move(value));
        return atom({.op = Op::charclass, .index = static_cast<int>(classes_.size() - 1)});
    }

    std::vector<char32_t> pattern_;
    std::vector<State> states_;
    std::vector<CharClass> classes_;
    std::size_t cursor_{};
    int start_{-1};
    int group_count_{};
    bool sensitive_;
};

[[nodiscard]] inline auto fuzzy_at(const Input& text, const std::size_t offset,
                                   const Input& needle, const bool sensitive) -> std::optional<std::pair<std::size_t, int>> {
    const auto equal = [sensitive](char32_t a, char32_t b) { return sensitive ? a == b : fold(a) == fold(b); };
    if (offset == text.values.size() || !equal(text.values[offset], needle.values.front())) return std::nullopt;
    auto cursor = offset;
    int score = 0;
    for (const auto cp : needle.values) {
        while (cursor < text.values.size() && !equal(cp, text.values[cursor])) {
            ++cursor;
            if (score > -1000000) --score;
        }
        if (cursor == text.values.size()) return std::nullopt;
        if (score < 1000000) score += cursor == 0 || !word(text.values[cursor - 1]) ? 10 : 1;
        ++cursor;
    }
    while (!text.breaks[cursor]) ++cursor;
    return std::pair{cursor, score};
}

struct Located { Match match; std::size_t first, last; };

[[nodiscard]] inline auto scan(const textbuf::Snapshot& snapshot, const Query& query, bool single)
    -> std::vector<Located> {
    std::vector<Located> matches;
    const auto text = snapshot.text();
    const auto index = snapshot.line_index();
    if (query.text().empty()) return matches;
    const auto start = index.offset({}, text);
    const auto add = [&](std::size_t first, std::size_t last, int score, auto captures) {
        const auto begin = index.position(first, text), end = index.position(last, text);
        matches.push_back({{begin, end, score, {begin, end}, std::move(captures)}, first, last});
    };
    using Captures = std::vector<std::optional<std::string>>;
    if (query.kind() == Query::Kind::literal && query.case_sensitive()) {
        // Boyer–Moore–Horspool for byte-exact queries. Check that candidate
        // endpoints can be represented by logical grapheme positions.
        const auto needle = query.text();
        std::array<std::size_t, 256> shift;
        shift.fill(needle.size());
        for (std::size_t i = 0; i + 1 < needle.size(); ++i)
            shift[static_cast<unsigned char>(needle[i])] = needle.size() - 1 - i;
        for (auto at = start; needle.size() <= text.size() - at;) {
            const auto end = at + needle.size();
            if (text.substr(at, needle.size()) == needle &&
                index.offset(index.position(at, text), text) == at &&
                index.offset(index.position(end, text), text) == end) {
                add(at, end, 0, Captures{});
                if (single) break;
                at = end;
            } else at += shift[static_cast<unsigned char>(text[end - 1])];
        }
        return matches;
    }
    const Input input{text, start};
    const Input needle{query.text(), 0};
    const auto regex = query.kind() == Query::Kind::regex ? std::optional<Regex>{std::in_place, query.text(),
        query.case_sensitive()} : std::nullopt;
    if (regex && !regex->valid()) return matches;
    Regex::Failures failures;
    for (std::size_t at = 0; at <= input.values.size();) {
        if (!input.breaks[at]) { ++at; continue; }
        std::optional<std::size_t> last;
        int score = 0;
        Captures captures;
        if (regex) {
            if (const auto result = regex->run(input, at, &failures)) {
                last = result->end;
                captures.emplace_back(text.substr(input.bytes[at], input.bytes[*last] - input.bytes[at]));
                for (std::size_t i = 2; i < result->captures.size(); i += 2) {
                    const auto first = result->captures[i], end = result->captures[i + 1];
                    if (first == Regex::no_capture || end == Regex::no_capture || end < first)
                        captures.push_back(std::nullopt);
                    else captures.emplace_back(text.substr(input.bytes[first], input.bytes[end] - input.bytes[first]));
                }
            }
        } else if (query.kind() == Query::Kind::literal) {
            if (needle.values.size() <= input.values.size() - at && input.breaks[at + needle.values.size()] &&
                std::equal(needle.values.begin(), needle.values.end(), input.values.begin() + at,
                           [](auto left, auto right) { return fold(left) == fold(right); }))
                last = at + needle.values.size();
        } else if (const auto fuzzy = fuzzy_at(input, at, needle, query.case_sensitive())) {
            last = fuzzy->first;
            score = fuzzy->second;
        }
        if (last) {
            add(input.bytes[at], input.bytes[*last], score, std::move(captures));
            if (single) break;
        }
        at = last && *last > at ? *last : at + 1;
    }
    return matches;
}

}  // namespace detail

[[nodiscard]] inline auto find_all(const textbuf::Snapshot& snapshot, const Query& query) -> std::vector<Match> {
    std::vector<Match> matches;
    for (auto& match : detail::scan(snapshot, query, false)) matches.push_back(std::move(match.match));
    return matches;
}

[[nodiscard]] inline auto find(const textbuf::Snapshot& snapshot, const Query& query) -> std::optional<Match> {
    auto matches = detail::scan(snapshot, query, true);
    if (matches.empty()) return std::nullopt;
    return std::move(matches.front().match);
}

/** Safe stateless entry point when the previous query and revision are unknown. */
[[nodiscard]] inline auto incremental(const textbuf::Snapshot& snapshot, const Query& query,
                                      const std::vector<Match>& previous = {}) -> std::vector<Match> {
    (void)previous;
    return find_all(snapshot, query);
}

/** Track all candidate starts while a case-sensitive literal query grows.
 *  Unlike a public match list, the candidate set retains overlapping prefixes.
 */
class IncrementalState {
public:
    [[nodiscard]] auto update(const textbuf::Snapshot& snapshot, const Query& query) -> std::vector<Match> {
        if (query.kind() != Query::Kind::literal || !query.case_sensitive() || query.text().empty()) {
            previous_.clear();
            candidates_.clear();
            return find_all(snapshot, query);
        }
        const auto text = snapshot.text();
        const auto needle = query.text();
        const auto index = snapshot.line_index();
        if (previous_.empty() || snapshot.generation() != snapshot_.generation() ||
            text.data() != snapshot_.text().data() || !needle.starts_with(previous_)) {
            candidates_.clear();
            for (auto at = text.find(needle, index.offset({}, text)); at != std::string_view::npos;
                 at = text.find(needle, at + 1))
                if (index.offset(index.position(at, text), text) == at) candidates_.push_back(at);
        } else {
            std::erase_if(candidates_, [&](std::size_t at) { return text.substr(at, needle.size()) != needle; });
        }
        snapshot_ = snapshot;
        previous_ = needle;
        std::vector<Match> matches;
        std::size_t next = 0;
        for (const auto at : candidates_) {
            if (at < next) continue;
            const auto final = at + needle.size();
            const auto begin = index.position(at, text), end = index.position(final, text);
            if (index.offset(end, text) != final) continue;
            matches.push_back({begin, end, 0, {begin, end}, {}});
            next = final;
        }
        return matches;
    }

private:
    textbuf::Snapshot snapshot_{};
    std::string previous_;
    std::vector<std::size_t> candidates_;
};

[[nodiscard]] inline auto incremental(const textbuf::Snapshot& snapshot, const Query& query,
                                      IncrementalState& state) -> std::vector<Match> {
    return state.update(snapshot, query);
}

class Replacer {
public:
    Replacer(textbuf::Buffer& buffer, Query query, std::string replacement)
        : buffer_(&buffer), query_(std::move(query)), replacement_(std::move(replacement)) {}

    /** Confirm in document order and apply accepted edits without shifting later ranges.
     *  For regex queries `$0` to `$99` or backslash-number refer to captures; `$$` emits a dollar.
     */
    template <typename Confirm>
    auto confirm_each(Confirm&& confirm) -> std::size_t {
        const auto snapshot = buffer_->snapshot();
        const auto matches = detail::scan(snapshot, query_, false);
        std::vector<std::pair<const detail::Located*, std::string>> accepted;
        for (const auto& located : matches) {
            if (std::invoke(confirm, located.match)) accepted.emplace_back(&located, expand(located.match));
            if (buffer_->generation() != snapshot.generation()) return 0;
        }
        for (auto it = accepted.rbegin(); it != accepted.rend(); ++it)
            buffer_->replace_bytes(it->first->first, it->first->last, it->second);
        return accepted.size();
    }

    auto replace_all() -> std::size_t {
        return confirm_each([](const Match&) { return true; });
    }

private:
    [[nodiscard]] auto expand(const Match& match) const -> std::string {
        if (query_.kind() != Query::Kind::regex) return replacement_;
        std::string output;
        for (std::size_t at = 0; at < replacement_.size(); ++at) {
            const auto c = replacement_[at];
            if ((c == '$' || c == '\\') && at + 1 < replacement_.size()) {
                const auto next = replacement_[at + 1];
                if (next == c) { output += c; ++at; continue; }
                if (next >= '0' && next <= '9') {
                    auto group = static_cast<std::size_t>(next - '0');
                    ++at;
                    if (at + 1 < replacement_.size() && replacement_[at + 1] >= '0' && replacement_[at + 1] <= '9')
                        group = group * 10 + static_cast<std::size_t>(replacement_[++at] - '0');
                    if (group < match.captures.size() && match.captures[group]) output += *match.captures[group];
                    continue;
                }
            }
            output += c;
        }
        return output;
    }
    textbuf::Buffer* buffer_{};
    Query query_;
    std::string replacement_;
};

}  // namespace ttyutils::search
