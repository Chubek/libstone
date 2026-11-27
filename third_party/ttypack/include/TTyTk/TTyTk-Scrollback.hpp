#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

namespace ttytk {

/** @brief Bounded history, ordered from oldest to newest. */
class Scrollback {
public:
    /** @brief Store a row. Set wraps when the next row continues this logical line. */
    void push_line(std::vector<Cell> line, bool wraps = false) {
        if (limit_ == 0) return;
        const auto columns = line.size();
        if (!wraps) while (!line.empty() && line.back() == Cell{}) line.pop_back();
        const auto used = line.size();
        push(lines_, store(std::move(line), columns, used, wraps));
    }

    /** @brief Read a row, lazily expanding any omitted trailing blank cells. */
    [[nodiscard]] auto line(std::size_t index) const -> const std::vector<Cell>& {
        const auto& row = lines_.at(index);
        row.cells.resize(row.columns);
        return row.cells;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return lines_.size(); }

    void set_limit(std::size_t limit) {
        limit_ = limit;
        while (lines_.size() > limit_) lines_.pop_front();
    }

    /** @brief Rewrap logical lines without splitting wide glyphs.
     * A zero width leaves history unchanged; at width one a wide glyph keeps
     * both cells on a row of its own. Reflow-created padding is not content.
     */
    void reflow(std::size_t columns) {
        if (columns == 0 || lines_.empty()) return;
        std::deque<StoredLine> reflowed;
        std::vector<Cell> row;
        const auto emit = [&](bool wraps) {
            const auto used = row.size();
            push(reflowed, store(std::exchange(row, {}), std::max(columns, used), used, wraps));
        };
        for (const auto& source : lines_) {
            for (std::size_t i = 0; i < source.used;) {
                const auto width = i + 1 < source.used && source.cells[i + 1].continuation ? 2U : 1U;
                if (!row.empty() && (row.size() >= columns || width > columns - row.size()))
                    emit(true);
                row.push_back(source.cells[i++]);
                if (width == 2) row.push_back(source.cells[i++]);
            }
            if (!source.wraps) emit(false);
        }
        if (!row.empty() || lines_.back().wraps) emit(lines_.back().wraps);
        lines_ = std::move(reflowed);
    }

private:
    struct StoredLine {
        mutable std::vector<Cell> cells;
        std::size_t columns, used;
        bool wraps;
    };

    [[nodiscard]] static auto store(std::vector<Cell> row, std::size_t columns, std::size_t used,
                                    bool wraps) -> StoredLine {
        // Copy only the used prefix when the input reserved capacity for blank cells.
        if (row.capacity() > row.size()) {
            std::vector<Cell> compact(row.begin(), row.end());
            row.swap(compact);
        }
        return {std::move(row), columns, used, wraps};
    }

    void push(std::deque<StoredLine>& target, StoredLine row) const {
        target.push_back(std::move(row));
        if (target.size() > limit_) target.pop_front();
    }

    std::deque<StoredLine> lines_{};
    std::size_t limit_{10000};
};

} // namespace ttytk
