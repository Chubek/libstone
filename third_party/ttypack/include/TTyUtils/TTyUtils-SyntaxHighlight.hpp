#pragma once

#include <TTyUtils/TTyUtils-Search.hpp>
#include <TTyUtils/TTyUtils-SyntaxGrammar.hpp>
#include <TTyUtils/TTyUtils-TextBuffer.hpp>
#include <TTyUtils/TTyUtils-Theme.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::hl {

class SpanList {
public:
    struct Item {
        std::size_t begin{};
        std::size_t end{};
        syntax::Scope scope{};
    };

    [[nodiscard]] auto items() const noexcept -> const std::vector<Item>& { return items_; }
    void add(Item item) { items_.push_back(std::move(item)); }

private:
    std::vector<Item> items_;
};

struct LineState {
    std::size_t generation{};
    bool lexical_only{};
    std::size_t open_rule{std::numeric_limits<std::size_t>::max()};
};

class Incremental {
public:
    void invalidate_from(const std::size_t line) noexcept { first_dirty_ = std::min(first_dirty_, line); }
    [[nodiscard]] auto first_dirty() const noexcept -> std::size_t { return first_dirty_; }
    void clean() noexcept { first_dirty_ = static_cast<std::size_t>(-1); }

private:
    std::size_t first_dirty_{static_cast<std::size_t>(-1)};
};

/** Return a scope's stable theme role name. */
[[nodiscard]] inline auto to_style(const syntax::Scope& scope) -> std::string {
    return std::string(scope.name());
}

/** Resolve a scope against the active theme. */
[[nodiscard]] inline auto to_style(const syntax::Scope& scope, const theme::Theme& theme) -> theme::Style {
    return theme.role(scope.name());
}

class Highlighter {
public:
    explicit Highlighter(const syntax::Grammar& grammar) : grammar_(grammar) {
        for (const auto& rule : grammar_.rules) {
            const auto multi = rule.match.empty();
            search::detail::Regex first{multi ? rule.begin : rule.match, true};
            std::optional<search::detail::Regex> end;
            if (multi) end.emplace(rule.end, true);
            if (first.valid() && (!multi || end->valid()))
                rules_.push_back({std::move(first), std::move(end), rule.scope});
        }
    }

    Highlighter(const syntax::Grammar& grammar, const theme::Theme& theme) : Highlighter(grammar) {
        theme_ = &theme;
    }

    void attach(textbuf::Buffer& buffer) {
        buffer_ = &buffer;
        snapshot_ = buffer.snapshot();
        cache_.clear();
        dirty_ = 0;
    }

    [[nodiscard]] auto line_spans(const std::size_t line) -> SpanList {
        if (!buffer_ || line >= buffer_->line_index().lines()) return {};
        if (snapshot_.generation() != buffer_->generation()) {
            const auto current = buffer_->snapshot();
            const auto count = std::min(cache_.size(), current.line_index().lines());
            std::size_t first = 0;
            // Preserve the cached prefix; each later line is reused if both
            // the source and the preceding line's lexical state agree.
            while (first < count && snapshot_.line(first) == current.line(first)) ++first;
            dirty_ = std::min(dirty_, first);
            cache_.resize(count);
            snapshot_ = current;
        }
        if (line >= cache_.size()) cache_.resize(line + 1);
        auto first = no_rule, last = std::size_t{0};
        for (; dirty_ <= line; ++dirty_) {
            auto& entry = cache_[dirty_];
            const auto source = snapshot_.line(dirty_);
            const auto incoming = dirty_ == 0 ? no_rule : cache_[dirty_ - 1].state.open_rule;
            if (entry.present && entry.incoming == incoming && entry.source == source) {
                entry.state.generation = snapshot_.generation();
                continue;
            }
            entry = lex(source, incoming, snapshot_.generation());
            first = std::min(first, dirty_);
            last = dirty_;
        }
        auto result = cache_[line].spans;
        if (ready_ && first != no_rule) {
            const auto callback = ready_;
            const auto bounded = [](std::size_t n) { return static_cast<int>(std::min(n,
                static_cast<std::size_t>(std::numeric_limits<int>::max()))); };
            callback(bounded(first), bounded(last));
        }
        return result;
    }

    void on_ready(std::function<void(int, int)> callback) { ready_ = std::move(callback); }
    void set_line_limit(const std::size_t limit) noexcept {
        if (limit != line_limit_) { line_limit_ = limit; cache_.clear(); dirty_ = 0; }
    }
    [[nodiscard]] auto style(const syntax::Scope& scope) const -> theme::Style {
        return theme_ ? to_style(scope, *theme_) : theme::Style{};
    }

private:
    static constexpr auto no_rule = std::numeric_limits<std::size_t>::max();
    struct Compiled {
        search::detail::Regex first;
        std::optional<search::detail::Regex> end;
        syntax::Scope scope;
    };
    struct Cached {
        bool present;
        std::string source;
        std::size_t incoming;
        LineState state;
        SpanList spans;
    };

    [[nodiscard]] auto lex(std::string_view source, std::size_t incoming, std::size_t generation) const -> Cached {
        const auto lexical = source.size() > line_limit_;
        auto open = incoming;
        Cached result{true, std::string(source), incoming, {generation, lexical, open}, {}};
        const search::detail::Input input{source, 0};
        std::vector<SpanList::Item> spans;
        std::size_t at = 0, span = 0;
        if (!lexical) {
            std::size_t budget = 100000;
            std::vector<search::detail::Regex::Failures> start_failures(rules_.size()), end_failures(rules_.size());
            while (at <= input.values.size() && budget > 0) {
                if (!input.breaks[at]) { ++at; continue; }
                if (open != no_rule) {
                    auto end = rules_[open].end->run(input, at, &end_failures[open], &budget);
                    if (!end) { ++at; continue; }
                    if (input.bytes[end->end] > span) spans.push_back({span, input.bytes[end->end], rules_[open].scope});
                    open = no_rule;
                    if (end->end > at) { at = end->end; continue; }
                }
                bool matched = false;
                for (std::size_t i = 0; i < rules_.size() && budget > 0; ++i) {
                    const auto& rule = rules_[i];
                    auto found = rule.first.run(input, at, &start_failures[i], &budget);
                    if (!found || found->end == at) continue;
                    span = input.bytes[at];
                    if (rule.end) open = i;
                    else spans.push_back({span, input.bytes[found->end], rule.scope});
                    at = found->end;
                    matched = true;
                    break;
                }
                if (!matched) ++at;
            }
            result.state.lexical_only = budget == 0;
        }
        if (open != no_rule && span < source.size()) spans.push_back({span, source.size(), rules_[open].scope});
        result.state.open_rule = open;

        // Lexical tokens are also available above the regex budget. Add them
        // only where a grammar pattern hasn't claimed the bytes.
        std::vector<SpanList::Item> keywords;
        std::size_t pattern = 0;
        for (std::size_t i = 0; i < input.values.size();) {
            if (!search::detail::word(input.values[i])) { ++i; continue; }
            const auto begin = input.bytes[i];
            do { ++i; } while (i < input.values.size() && search::detail::word(input.values[i]));
            const auto end = input.bytes[i];
            while (pattern < spans.size() && spans[pattern].end <= begin) ++pattern;
            if ((pattern == spans.size() || spans[pattern].begin >= end) && input.breaks[i] &&
                std::find(grammar_.keywords.begin(), grammar_.keywords.end(), source.substr(begin, end - begin)) !=
                    grammar_.keywords.end())
                keywords.push_back({begin, end, grammar_.root_scope});
        }
        std::size_t a = 0, b = 0;
        while (a < spans.size() || b < keywords.size()) {
            if (b == keywords.size() || (a < spans.size() && spans[a].begin <= keywords[b].begin))
                result.spans.add(std::move(spans[a++]));
            else result.spans.add(std::move(keywords[b++]));
        }
        return result;
    }

    syntax::Grammar grammar_;
    std::vector<Compiled> rules_;
    const theme::Theme* theme_{};
    textbuf::Buffer* buffer_{};
    textbuf::Snapshot snapshot_{};
    std::vector<Cached> cache_;
    std::function<void(int, int)> ready_{};
    std::size_t dirty_{};
    std::size_t line_limit_{4096};
};

}  // namespace ttyutils::hl
