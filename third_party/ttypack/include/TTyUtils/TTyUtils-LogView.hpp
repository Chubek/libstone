#pragma once

#include <TTyUtils/TTyUtils-EventLoop.hpp>
#include <TTyUtils/TTyUtils-Pager.hpp>
#include <TTyUtils/TTyUtils-Search.hpp>

#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

namespace ttyutils::logview {

struct Rule {
    std::string pattern{};
    theme::Style style{};
};

namespace detail {

[[nodiscard]] inline auto compile(const std::string& pattern) -> std::optional<std::regex> {
    try { return std::regex(pattern, std::regex::optimize); }
    catch (const std::regex_error&) { return {}; }
}

[[nodiscard]] inline auto matches(const std::string_view text, const std::regex& expression) -> bool {
    try { return std::regex_search(text.begin(), text.end(), expression); }
    catch (const std::regex_error&) { return false; }
}

}  // namespace detail

class Filter {
public:
    enum class Mode { Include, Exclude };
    Filter() = default;
    Filter(const Mode mode, std::string pattern) : mode_(mode), expression_(detail::compile(pattern)) {}
    [[nodiscard]] static auto include(std::string pattern) -> Filter { return {Mode::Include, std::move(pattern)}; }
    [[nodiscard]] static auto exclude(std::string pattern) -> Filter { return {Mode::Exclude, std::move(pattern)}; }
    /// Invalid filters are ignored, rather than accidentally hiding all rows.
    [[nodiscard]] auto valid() const noexcept -> bool { return expression_.has_value(); }
    [[nodiscard]] auto accepts(const std::string_view text) const -> bool {
        if (!expression_) return true;
        const auto found = detail::matches(text, *expression_);
        return mode_ == Mode::Include ? found : !found;
    }

private:
    Mode mode_{Mode::Include};
    std::optional<std::regex> expression_{std::regex{""}};
};

/// Opens a regular file for incremental polling by the resulting LogView.
inline auto tail_file(const std::filesystem::path& path) -> pager::Source {
    auto source = pager::Source::file(path);
    (void)source.poll();
    return source;
}

class LogView : public pager::Pager {
public:
    explicit LogView(pager::Source source = {}) : pager::Pager(std::move(source)) { follow(true); }
    /// Add a coloring rule; the first matching rule wins.
    void add_rule(Rule rule) {
        if (auto expression = detail::compile(rule.pattern)) {
            rules_.push_back({std::move(*expression), std::move(rule.style)});
            invalidate(rect());
        }
    }
    void set_filter(Filter filter) { filter_ = std::move(filter); pending_ = 0; filter_changed(); }
    [[nodiscard]] auto pending_lines() const noexcept -> std::size_t { return following() ? 0 : pending_; }
    void resume() { pending_ = 0; follow(true); invalidate(rect()); }
    void paint(ui::Canvas& canvas) override {
        pager::Pager::paint(canvas);
        const auto area = rect();
        if (area.empty() || !pending_lines()) return;
        auto banner = std::to_string(pending_lines()) + " new lines";
        banner.resize(static_cast<std::size_t>(area.width), ' ');
        auto banner_style = style();
        banner_style.attributes |= 1U << 6; // Inverse.
        canvas.text(area.x, area.y + area.height - 1, banner, banner_style);
    }

protected:
    [[nodiscard]] auto accepts_line(const std::string_view text) const -> bool override {
        return filter_.accepts(text);
    }
    [[nodiscard]] auto line_style(const std::string_view text) const -> std::optional<theme::Style> override {
        for (const auto& rule : rules_)
            if (detail::matches(text, rule.first)) return rule.second;
        return {};
    }
    void source_changed(const std::size_t previous, const bool restarted) override {
        if (following()) pending_ = 0;
        else if (restarted) pending_ = length();
        else if (length() > previous) pending_ += length() - previous;
    }

private:
    std::vector<std::pair<std::regex, theme::Style>> rules_;
    Filter filter_;
    std::size_t pending_{};
};

}  // namespace ttyutils::logview
