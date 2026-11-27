#pragma once

#include <TTyTk/TTyTk-Grampheme.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>
#include <TTyUtils/TTyUtils-Config.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ttyutils::keymap {

struct Chord {
    std::vector<std::string> keys{};
};

using Action = std::string;

[[nodiscard]] inline auto parse_key(const std::string_view notation) -> Chord {
    Chord chord;
    constexpr std::string_view separators{" \t\n\r"};
    for (auto start = notation.find_first_not_of(separators); start != std::string_view::npos;) {
        const auto end = notation.find_first_of(separators, start);
        auto key = notation.substr(start, end == std::string_view::npos ? end : end - start);
        if (key.starts_with("<leader>")) {
            chord.keys.emplace_back("<leader>");
            key.remove_prefix(8);
            for (const auto grapheme : ttytk::grapheme::segment(key)) chord.keys.emplace_back(grapheme);
        } else {
            chord.keys.emplace_back(key);
        }
        if (end == std::string_view::npos) break;
        start = notation.find_first_not_of(separators, end);
    }
    return chord;
}

class WhichKey {
public:
    void set(std::vector<std::string> hints) { hints_ = std::move(hints); }
    [[nodiscard]] auto hints() const noexcept -> const std::vector<std::string>& { return hints_; }

private:
    std::vector<std::string> hints_;
};

class Map {
public:
    using Clock = std::chrono::steady_clock;

    void bind(const std::string_view notation, Action action) { bind({}, notation, std::move(action)); }
    void bind(const std::string_view mode, const std::string_view notation, Action action) {
        const auto keys = expanded(notation);
        if (keys.empty()) return;
        auto& table = bindings_[std::string(mode)];
        if (action.empty()) table.erase(keys);
        else table[keys] = std::move(action);
    }
    [[nodiscard]] auto lookup(const std::string_view notation) const -> std::optional<Action> {
        const auto candidates = effective();
        const auto found = candidates.find(expanded(notation));
        return found == candidates.end() ? std::nullopt : std::optional{found->second};
    }
    void on_pending(std::function<void(std::vector<std::string>)> callback) { pending_ = std::move(callback); }

    void set_mode(std::string mode) {
        if (mode_ != mode) { mode_ = std::move(mode); reset(); }
    }
    [[nodiscard]] auto mode() const noexcept -> std::string_view { return mode_; }
    void set_timeout(const std::chrono::milliseconds timeout) noexcept {
        timeout_ = std::max(timeout, std::chrono::milliseconds::zero());
    }

    /// Selects the leader substituted into subsequently bound chords and input.
    void set_leader(const std::string_view notation) {
        auto keys = parse_key(notation).keys;
        if (keys.size() == 1 && keys.front() != "<leader>") leader_ = std::move(keys.front());
        reset();
    }

    /// Loads string-valued settings of the form `prefix.action = "key chord"`.
    void bind_config(const config::Store& store, const std::string_view prefix,
                     const std::string_view mode = {}) {
        auto path = std::string(prefix);
        if (!path.empty() && !path.ends_with('.')) path += '.';
        std::map<Action, std::string> sorted;
        for (const auto& [key, value] : store.values())
            if (key.starts_with(path) && key.size() > path.size())
                if (const auto* notation = std::get_if<std::string>(&value.storage()))
                    sorted.emplace(key.substr(path.size()), *notation);
        for (const auto& [action, notation] : sorted) bind(mode, notation, action);
    }

    /// Advances a chord by one key. A mismatch retries the new key by itself.
    /// If an exact binding is also a prefix, it resolves on timeout via expire().
    [[nodiscard]] auto feed(const std::string_view notation, const Clock::time_point now = Clock::now())
        -> std::optional<Action> {
        const auto keys = expanded(notation);
        if (keys.size() != 1) return std::nullopt;
        if (!chord_.empty() && now >= deadline_) reset();
        auto prefix = chord_;
        prefix.push_back(keys[0]);
        auto candidates = effective();
        auto hints = next_keys(candidates, prefix);
        auto exact = candidates.find(prefix);
        if (hints.empty() && exact == candidates.end() && !chord_.empty()) {
            prefix = keys;
            hints = next_keys(candidates, prefix);
            exact = candidates.find(prefix);
        }
        const auto action = exact == candidates.end() ? std::nullopt : std::optional{exact->second};
        if (hints.empty()) {
            reset();
            return action;
        }
        chord_ = std::move(prefix);
        deadline_ = now + timeout_;
        deferred_ = action;
        publish(std::move(hints));
        return std::nullopt;
    }

    [[nodiscard]] auto expire(const Clock::time_point now = Clock::now()) -> std::optional<Action> {
        if (chord_.empty() || now < deadline_) return std::nullopt;
        auto action = std::move(deferred_);
        reset();
        return action;
    }
    [[nodiscard]] auto which_key() const noexcept -> const WhichKey& { return hints_; }
    void reset() {
        chord_.clear();
        deferred_.reset();
        if (!hints_.hints().empty()) publish({});
    }

private:
    using Keys = std::vector<std::string>;
    using Bindings = std::map<Keys, Action>;

    void publish(std::vector<std::string> hints) {
        hints_.set(hints);
        if (pending_) pending_(std::move(hints));
    }

    [[nodiscard]] auto expanded(const std::string_view notation) const -> Keys {
        auto keys = parse_key(notation).keys;
        for (auto& key : keys) if (key == "<leader>") key = leader_;
        return keys;
    }

    [[nodiscard]] auto effective() const -> Bindings {
        Bindings result;
        if (const auto common = bindings_.find(""); common != bindings_.end()) result = common->second;
        if (!mode_.empty())
            if (const auto selected = bindings_.find(mode_); selected != bindings_.end())
                for (const auto& [keys, action] : selected->second) result[keys] = action;
        return result;
    }

    [[nodiscard]] static auto next_keys(const Bindings& bindings, const Keys& prefix) -> Keys {
        Keys result;
        for (auto it = bindings.lower_bound(prefix); it != bindings.end(); ++it) {
            const auto& keys = it->first;
            if (keys.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), keys.begin())) break;
            if (keys.size() > prefix.size() && (result.empty() || result.back() != keys[prefix.size()]))
                result.push_back(keys[prefix.size()]);
        }
        return result;
    }

    std::map<std::string, Bindings> bindings_;
    std::function<void(std::vector<std::string>)> pending_{};
    std::string mode_;
    std::string leader_{"Space"};
    Keys chord_;
    WhichKey hints_;
    std::optional<Action> deferred_;
    std::chrono::milliseconds timeout_{500};
    Clock::time_point deadline_{};
};

}  // namespace ttyutils::keymap
