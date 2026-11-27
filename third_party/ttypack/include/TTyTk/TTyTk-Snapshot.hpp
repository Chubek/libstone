#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>

#include <cstddef>
#include <string>

namespace ttytk::snapshot {

/** @brief An owning, read-only view of a captured terminal frame. */
class State {
public:
    State() = default;
    explicit State(const CellBuffer& buffer) : cells_(buffer) {}

    [[nodiscard]] auto rows() const noexcept -> std::size_t { return cells_.rows(); }
    [[nodiscard]] auto columns() const noexcept -> std::size_t { return cells_.columns(); }
    [[nodiscard]] auto at(std::size_t row, std::size_t column) const -> const Cell& {
        return cells_.at(row, column);
    }

    [[nodiscard]] auto cells() const noexcept -> const CellBuffer& { return cells_; }

private:
    CellBuffer cells_;
};

[[nodiscard]] inline auto capture(const CellBuffer& buffer) -> State { return State{buffer}; }

/** @brief Describe the first differing cell or dimension; empty on equality. */
[[nodiscard]] inline auto diff(const CellBuffer& lhs, const CellBuffer& rhs) -> std::string {
    if (lhs.rows() != rhs.rows() || lhs.columns() != rhs.columns()) return "size differs";
    if (lhs.columns() == 0) return {};
    for (std::size_t row = 0; row < lhs.rows(); ++row) {
        for (std::size_t col = 0; col < lhs.columns(); ++col) {
            if (lhs.at(row, col) != rhs.at(row, col)) {
                return "cell differs at (" + std::to_string(row) + ", " + std::to_string(col) + ")";
            }
        }
    }
    return {};
}

[[nodiscard]] inline auto diff(const State& lhs, const State& rhs) -> std::string {
    return diff(lhs.cells(), rhs.cells());
}

[[nodiscard]] inline auto diff(const CellBuffer& lhs, const State& rhs) -> std::string {
    return diff(lhs, rhs.cells());
}

[[nodiscard]] inline auto diff(const State& lhs, const CellBuffer& rhs) -> std::string {
    return diff(lhs.cells(), rhs);
}

/** @brief Serialize dimensions and every cell, using ASCII safe for golden files. */
[[nodiscard]] inline auto serialize(const CellBuffer& state) -> std::string {
    constexpr char digits[] = "0123456789abcdef";
    std::string out = "TTyTk snapshot v1 " + std::to_string(state.rows()) + ' ' +
                      std::to_string(state.columns()) + '\n';
    const auto byte = [&](unsigned value) {
        out.push_back(digits[(value >> 4U) & 0xFU]);
        out.push_back(digits[value & 0xFU]);
    };
    const auto color = [&](const color::Rgb rgb) {
        byte(rgb.red);
        byte(rgb.green);
        byte(rgb.blue);
    };
    if (state.columns() == 0) return out;
    for (std::size_t row = 0; row < state.rows(); ++row) {
        for (std::size_t col = 0; col < state.columns(); ++col) {
            const auto& cell = state.at(row, col);
            out += cell.continuation ? "+ " : "- ";
            out += std::to_string(cell.attributes);
            out += ' ';
            color(cell.foreground);
            out += ' ';
            color(cell.background);
            out += ' ';
            for (const unsigned char value : cell.grapheme) byte(value);
            out += '\n';
        }
    }
    return out;
}

[[nodiscard]] inline auto serialize(const State& state) -> std::string {
    return serialize(state.cells());
}

} // namespace ttytk::snapshot
