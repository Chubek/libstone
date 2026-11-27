#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ttytk {

/** @brief A dirty row span with inclusive column coordinates. */
struct DamageRegion {
    std::size_t row{}, first{}, last{};
    friend auto operator==(const DamageRegion&, const DamageRegion&) -> bool = default;
};

/** @brief Sorted, coalesced damage spans for a terminal frame. */
class Damage {
public:
    /** @brief Mark an inclusive span; reversed endpoints are accepted. */
    void mark(std::size_t row, std::size_t first, std::size_t last) {
        if (first > last) std::swap(first, last);
        auto start = std::lower_bound(dirty_.begin(), dirty_.end(), DamageRegion{row, first, last},
            [](const DamageRegion& lhs, const DamageRegion& rhs) {
                return lhs.row < rhs.row || (lhs.row == rhs.row && lhs.first < rhs.first);
            });
        if (start != dirty_.begin() && touches(*(start - 1), {row, first, last})) --start;
        auto stop = start;
        DamageRegion span{row, first, last};
        while (stop != dirty_.end() && stop->row == row &&
               (stop->first <= span.last || stop->first - span.last == 1)) {
            span.first = std::min(span.first, stop->first);
            span.last = std::max(span.last, stop->last);
            ++stop;
        }
        if (start == stop) dirty_.insert(start, span);
        else {
            *start = span;
            dirty_.erase(start + 1, stop);
        }
    }

    /** @brief Replace the damage set with one span covering each row. */
    void mark_all(std::size_t rows, std::size_t columns) {
        if (columns == 0) {
            clear();
            return;
        }
        dirty_.resize(rows);
        for (std::size_t row = 0; row < rows; ++row) dirty_[row] = {row, 0, columns - 1};
    }

    [[nodiscard]] auto regions() const noexcept -> const std::vector<DamageRegion>& { return dirty_; }
    void clear() noexcept { dirty_.clear(); }

    /** @brief Coalesce overlaps; mark() already maintains canonical spans. */
    void merge() noexcept {}

private:
    [[nodiscard]] static auto touches(DamageRegion lhs, DamageRegion rhs) noexcept -> bool {
        return lhs.row == rhs.row && (rhs.first <= lhs.last || rhs.first - lhs.last == 1);
    }

    std::vector<DamageRegion> dirty_{};
};

} // namespace ttytk
