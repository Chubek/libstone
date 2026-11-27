#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ttyutils::config {

[[nodiscard]] inline auto xdg_path(std::string_view kind) -> std::filesystem::path;

class Value {
public:
    using Storage = std::variant<std::monostate, bool, long long, double, std::string>;
    Value() = default;
    Value(bool value) : value_(value) {}
    template <std::integral T> requires (!std::same_as<T, bool>)
    Value(T value) {
        if constexpr (std::is_signed_v<T>) value_ = static_cast<long long>(value);
        else if (static_cast<std::uintmax_t>(value) <= std::numeric_limits<long long>::max())
            value_ = static_cast<long long>(value);
    }
    Value(double value) : value_(value) {}
    Value(std::string value) : value_(std::move(value)) {}
    Value(std::string_view value) : value_(std::string(value)) {}
    Value(const char* value) : value_(std::string(value ? value : "")) {}

    template <typename T>
    [[nodiscard]] auto as(const T& fallback = T{}) const -> T {
        if constexpr (std::integral<T> && !std::same_as<T, bool>) {
            if (const auto* value = std::get_if<long long>(&value_)) {
                if constexpr (std::is_signed_v<T>) {
                    if (*value >= std::numeric_limits<T>::lowest() && *value <= std::numeric_limits<T>::max())
                        return static_cast<T>(*value);
                } else if (*value >= 0 && static_cast<std::uintmax_t>(*value) <= std::numeric_limits<T>::max()) {
                    return static_cast<T>(*value);
                }
            }
        } else if constexpr (std::floating_point<T>) {
            if (const auto* value = std::get_if<double>(&value_)) return static_cast<T>(*value);
        } else if constexpr (std::same_as<T, bool> || std::same_as<T, std::string>) {
            if (const auto* value = std::get_if<T>(&value_)) return *value;
        }
        return fallback;
    }
    [[nodiscard]] auto empty() const noexcept -> bool { return std::holds_alternative<std::monostate>(value_); }
    [[nodiscard]] auto storage() const noexcept -> const Storage& { return value_; }
    [[nodiscard]] friend auto operator==(const Value&, const Value&) -> bool = default;

private:
    Storage value_;
};

struct Schema {
    enum class Type { Boolean = 1, Integer, Number, String };
    std::vector<std::string> required{};
    std::unordered_map<std::string, Type> types{};
    [[nodiscard]] auto valid(const std::unordered_map<std::string, Value>& values) const -> bool {
        for (const auto& key : required) {
            const auto found = values.find(key);
            if (found == values.end() || found->second.empty()) return false;
        }
        for (const auto& [key, type] : types)
            if (const auto found = values.find(key); found != values.end() &&
                found->second.storage().index() != static_cast<std::size_t>(type)) return false;
        return true;
    }
};

struct Diagnostic {
    std::filesystem::path file;
    std::size_t line{}, column{};
    std::string message;
};

class Store {
public:
    /// Loads system defaults, user overrides, then `<project>/<name>.toml`.
    [[nodiscard]] static auto load(const std::string& name, const std::filesystem::path& project = ".") -> Store {
        Store result;
        const auto filename = name + ".toml";
        const auto* configured = std::getenv("XDG_CONFIG_DIRS");
        auto directories = std::string_view(configured && *configured ? configured : "/etc/xdg");
        while (!directories.empty()) {
            const auto split = directories.find(':');
            const auto dir = std::filesystem::path(directories.substr(0, split));
            if (dir.is_absolute()) result.files_.push_back(dir / filename);
            if (split == std::string_view::npos) break;
            directories.remove_prefix(split + 1);
        }
        std::reverse(result.files_.begin(), result.files_.end());
        result.files_.push_back(xdg_path("config") / filename);
        if (!project.empty()) result.files_.push_back(project / filename);
        (void)result.reload();
        return result;
    }

    [[nodiscard]] static auto load_file(const std::filesystem::path& path) -> Store {
        Store result;
        result.files_.push_back(path);
        (void)result.reload();
        return result;
    }
    [[nodiscard]] auto get_value(const std::string_view key) const -> Value {
        const auto found = values_.find(std::string(key));
        return found == values_.end() ? Value{} : found->second;
    }
    template <typename T>
    [[nodiscard]] auto get(const std::string_view key, const T& fallback = T{}) const -> T {
        const auto it = values_.find(std::string(key));
        return it == values_.end() ? fallback : it->second.as<T>(fallback);
    }
    void set(std::string key, Value value) { values_[std::move(key)] = std::move(value); }
    [[nodiscard]] auto values() const noexcept -> const std::unordered_map<std::string, Value>& { return values_; }
    [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<Diagnostic>& { return diagnostics_; }

    /// Re-reads watched layers on demand; intended for a timer in an application loop.
    /// Observers run only when the effective parsed values change.
    auto reload() -> bool {
        if (files_.empty()) return false;
        auto previous = std::exchange(values_, {});
        diagnostics_.clear();
        for (const auto& file : files_) {
            std::ifstream input(file);
            if (input) parse(input, file);
        }
        if (previous == values_) return false;
        const auto count = callbacks_.size();
        for (std::size_t index = 0; index < count; ++index) callbacks_[index]();
        return true;
    }

private:
    friend void watch(Store&, std::function<void()>);

    [[nodiscard]] static auto trim(std::string_view text) noexcept -> std::string_view {
        constexpr std::string_view whitespace{" \t\r"};
        const auto first = text.find_first_not_of(whitespace);
        if (first == std::string_view::npos) return {};
        return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
    }

    [[nodiscard]] static auto uncomment(const std::string_view text) noexcept -> std::string_view {
        char quote{};
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto ch = text[i];
            if (quote == '"' && ch == '\\') { ++i; continue; }
            if (quote) { if (ch == quote) quote = 0; }
            else if (ch == '"' || ch == '\'') quote = ch;
            else if (ch == '#') return text.substr(0, i);
        }
        return text;
    }

    [[nodiscard]] static auto key_name(std::string_view key) -> std::string {
        std::string result;
        while (!key.empty()) {
            const auto dot = key.find('.');
            const auto field = trim(key.substr(0, dot));
            if (field.empty() || field.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
                                   != std::string_view::npos)
                return {};
            if (!result.empty()) result += '.';
            result += field;
            if (dot == std::string_view::npos) return result;
            key.remove_prefix(dot + 1);
        }
        return {};
    }

    [[nodiscard]] static auto scalar(const std::string_view value) -> std::optional<Value> {
        if (value.empty()) return std::nullopt;
        if (value == "true" || value == "false") return Value(value == "true");
        if (value.front() == '"' || value.front() == '\'') {
            const auto quote = value.front();
            std::string decoded;
            for (std::size_t i = 1; i < value.size(); ++i) {
                auto ch = value[i];
                if (ch == quote)
                    return i + 1 == value.size() ? std::optional<Value>{Value(std::move(decoded))} : std::nullopt;
                if (ch == '\\' && quote == '"') {
                    if (++i >= value.size()) break;
                    ch = value[i];
                    switch (ch) {
                    case 'n': ch = '\n'; break;
                    case 'r': ch = '\r'; break;
                    case 't': ch = '\t'; break;
                    case '"': case '\\': break;
                    default: return std::nullopt;
                    }
                }
                decoded += ch;
            }
            return std::nullopt;
        }
        const auto number = value.front() == '+' ? value.substr(1) : value;
        if (number.empty()) return std::nullopt;
        const auto end = number.data() + number.size();
        if (number.find_first_of("eE.") == std::string_view::npos) {
            long long integer{};
            const auto converted = std::from_chars(number.data(), end, integer);
            if (converted.ec == std::errc{} && converted.ptr == end) return Value(integer);
        } else {
            double decimal{};
            const auto converted = std::from_chars(number.data(), end, decimal);
            if (converted.ec == std::errc{} && converted.ptr == end) return Value(decimal);
        }
        return std::nullopt;
    }

    void parse(std::istream& input, const std::filesystem::path& file) {
        std::string line;
        std::string section;
        std::size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            const auto text = trim(uncomment(line));
            if (text.empty()) continue;
            const auto column = static_cast<std::size_t>(text.data() - line.data() + 1);
            if (text.starts_with('[')) {
                auto next = text.ends_with(']') ? key_name(text.substr(1, text.size() - 2)) : std::string{};
                if (next.empty()) diagnostics_.push_back({file, number, column, "invalid table name"});
                else section = std::move(next);
                continue;
            }
            const auto equal = text.find('=');
            auto key = equal == std::string_view::npos ? std::string{} : key_name(text.substr(0, equal));
            if (key.empty()) {
                diagnostics_.push_back({file, number, column, "expected a key and '='"});
                continue;
            }
            const auto field = trim(text.substr(equal + 1));
            if (auto value = scalar(field)) {
                if (!section.empty()) key = section + '.' + key;
                set(std::move(key), std::move(*value));
            } else {
                diagnostics_.push_back({file, number, field.empty() ? column + equal + 1 :
                    static_cast<std::size_t>(field.data() - line.data() + 1), "invalid scalar value"});
            }
        }
    }
    std::unordered_map<std::string, Value> values_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<std::filesystem::path> files_;
    std::deque<std::function<void()>> callbacks_;
};

/// Observes changes detected by Store::reload().
inline void watch(Store& store, std::function<void()> callback) {
    if (callback) store.callbacks_.push_back(std::move(callback));
}

inline auto xdg_path(const std::string_view kind) -> std::filesystem::path {
    const auto [variable, suffix] = kind == "config" ? std::pair{"XDG_CONFIG_HOME", ".config"} :
        kind == "cache" ? std::pair{"XDG_CACHE_HOME", ".cache"} :
        kind == "state" ? std::pair{"XDG_STATE_HOME", ".local/state"} :
        kind == "runtime" ? std::pair{"XDG_RUNTIME_DIR", ""} : std::pair{"XDG_DATA_HOME", ".local/share"};
    const char* base = std::getenv(variable);
    if (base && std::filesystem::path(base).is_absolute()) return base;
    if (kind == "runtime") return {};
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home && *home ? home : "."} / suffix;
}

}  // namespace ttyutils::config
#include <optional>
