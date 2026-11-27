#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>

namespace ttytk {

/** @brief Horizontal tab stops on zero-based columns, initially every eight columns. */
class Tabstops {
public:
    Tabstops() = default;
    /** @brief A tab past the available stops moves to the corresponding margin. */
    explicit Tabstops(std::size_t columns) noexcept : columns_(columns) {}

    void set(std::size_t column) {
        cleared_.erase(column);
        if (!is_default(column)) added_.insert(column);
    }

    void clear(std::size_t column) {
        if (is_default(column)) cleared_.insert(column);
        added_.erase(column);
    }

    void clear_all() noexcept {
        defaults_ = false;
        added_.clear();
        cleared_.clear();
    }

    /** @brief Restore the DECST8C pattern, starting at the ninth column. */
    void reset() noexcept {
        clear_all();
        defaults_ = true;
    }

    /** @brief Set the terminal width, preserving explicit settings. */
    void resize(std::size_t columns) noexcept { columns_ = columns; }

    [[nodiscard]] auto next(std::size_t column) const noexcept -> std::size_t {
        const auto margin = columns_ ? (*columns_ == 0 ? 0 : *columns_ - 1) : max_;
        if (column >= margin) return margin;
        auto stop = margin;
        if (const auto it = added_.upper_bound(column); it != added_.end())
            stop = std::min(stop, *it);
        if (defaults_) {
            auto candidate = column - column % 8;
            while (stop - candidate > 8) {
                candidate += 8;
                if (!cleared_.contains(candidate)) return candidate;
            }
        }
        return !columns_ && stop == max_ && !added_.contains(max_) ? column : stop;
    }

    [[nodiscard]] auto prev(std::size_t column) const noexcept -> std::size_t {
        if (columns_) column = std::min(column, *columns_ == 0 ? 0 : *columns_ - 1);
        auto stop = std::optional<std::size_t>{};
        if (auto it = added_.lower_bound(column); it != added_.begin()) stop = *--it;
        if (defaults_ && column != 0) {
            auto candidate = column - 1;
            candidate -= candidate % 8;
            while (candidate > stop.value_or(0)) {
                if (!cleared_.contains(candidate)) return candidate;
                candidate -= 8;
            }
        }
        return stop.value_or(columns_ ? 0 : column);
    }

private:
    [[nodiscard]] auto is_default(std::size_t column) const noexcept -> bool {
        return defaults_ && column != 0 && column % 8 == 0;
    }

    static constexpr auto max_ = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> columns_{};
    bool defaults_{true};
    std::set<std::size_t> added_{};
    std::set<std::size_t> cleared_{};
};

} // namespace ttytk
