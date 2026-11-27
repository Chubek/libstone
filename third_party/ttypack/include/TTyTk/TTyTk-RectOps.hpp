#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-CellBuffer.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ttytk::rect {

/** Zero-based inclusive rectangle in a CellBuffer. */
struct Region {
    std::size_t top{};
    std::size_t left{};
    std::size_t bottom{};
    std::size_t right{};
};

/** Convert an indexed four-coordinate object to a rectangle. */
template <typename RegionLike>
[[nodiscard]] constexpr auto region(const RegionLike& value) noexcept -> Region {
    return {static_cast<std::size_t>(value[0]), static_cast<std::size_t>(value[1]),
            static_cast<std::size_t>(value[2]), static_cast<std::size_t>(value[3])};
}

/** Return an existing native rectangle unchanged. */
[[nodiscard]] constexpr auto region(Region value) noexcept -> Region { return value; }

namespace detail {

struct Extent {
    std::size_t height{};
    std::size_t width{};
};

[[nodiscard]] inline auto extent(const CellBuffer& buffer, Region area) noexcept -> Extent {
    if (area.top >= buffer.rows() || area.left >= buffer.columns() ||
        area.bottom < area.top || area.right < area.left) return {};
    return {std::min(area.bottom, buffer.rows() - 1) - area.top + 1,
            std::min(area.right, buffer.columns() - 1) - area.left + 1};
}

}  // namespace detail

/**
 * Copy styled cells between clipped regions of the same page (DECCRA).
 * Overlap is safe. If a boundary cuts a wide glyph, copy a blank instead.
 */
inline void copy(CellBuffer& buffer, Region source, Region destination) {
    const auto src = detail::extent(buffer, source);
    const auto dst = detail::extent(buffer, destination);
    const auto height = std::min(src.height, dst.height);
    const auto width = std::min(src.width, dst.width);
    if (height == 0 || width == 0) return;

    std::vector<Cell> snapshot;
    snapshot.reserve(height * width);
    for (std::size_t row = 0; row < height; ++row) {
        for (std::size_t col = 0; col < width; ++col)
            snapshot.push_back(buffer.at(source.top + row, source.left + col));
        auto& first = snapshot[row * width];
        auto& last = snapshot[(row + 1) * width - 1];
        if (first.continuation) first = Cell{};
        const auto after = source.left + width;
        if (after < buffer.columns() && buffer.at(source.top + row, after).continuation)
            last = Cell{};
    }
    for (std::size_t row = 0; row < height; ++row)
        for (std::size_t col = 0; col < width; ++col) {
            auto& cell = snapshot[row * width + col];
            if (!cell.continuation)
                buffer.put(destination.top + row, destination.left + col, std::move(cell));
        }
}

/** Copy a region to a top-left destination, clipping at the page edge. */
inline void copy(CellBuffer& buffer, Region source, std::size_t top, std::size_t left) {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    copy(buffer, source, Region{top, left, max, max});
}

/** Copy using indexed four-coordinate objects. */
template <typename Source, typename Destination>
inline void copy(CellBuffer& buffer, const Source& source, const Destination& destination) {
    copy(buffer, region(source), region(destination));
}

/** Fill a clipped rectangle with a printable scalar and attributes, retaining colors. */
inline void fill(CellBuffer& buffer, Region area, char32_t character, std::uint32_t attributes) {
    const auto shape = detail::extent(buffer, area);
    const auto width = charwidth::of(character);
    if (shape.width == 0 || width == 0 || character > 0x10FFFF ||
        (character >= 0xD800 && character <= 0xDFFF)) return;
    const auto tile = utf8::encode(character);
    for (std::size_t row = 0; row < shape.height; ++row) {
        // Writing may clear part of a preexisting wide glyph. Capture this
        // row's colors before any of those writes.
        std::vector<std::pair<color::Rgb, color::Rgb>> colors;
        colors.reserve(shape.width);
        for (std::size_t col = 0; col < shape.width; ++col) {
            const auto& cell = buffer.at(area.top + row, area.left + col);
            colors.emplace_back(cell.foreground, cell.background);
        }
        for (std::size_t col = 0; col < shape.width;) {
            const auto fits = width == 1 || shape.width - col >= 2;
            const auto [foreground, background] = colors[col];
            buffer.put(area.top + row, area.left + col,
                       Cell{fits ? tile : " ", attributes, foreground, background});
            col += fits ? static_cast<std::size_t>(width) : 1U;
        }
    }
}

/** Fill using an indexed four-coordinate object. */
template <typename RegionLike>
inline void fill(CellBuffer& buffer, const RegionLike& area, char32_t character,
                 std::uint32_t attributes) {
    fill(buffer, region(area), character, attributes);
}

/** Erase a clipped rectangle to blanks without attributes, retaining colors (DECERA). */
inline void erase(CellBuffer& buffer, Region area) {
    fill(buffer, area, U' ', 0);
}

/** Erase using an indexed four-coordinate object. */
template <typename RegionLike>
inline void erase(CellBuffer& buffer, const RegionLike& area) {
    erase(buffer, region(area));
}

/**
 * Compute the default DECRQCRA-style 16-bit additive complement.
 * The primary scalar contributes its low 8 bits, and plain blanks and
 * continuation cells do not contribute. Page state and terminal extensions
 * are outside this primitive.
 */
[[nodiscard]] inline auto checksum(const CellBuffer& buffer, Region area) noexcept -> std::uint16_t {
    const auto shape = detail::extent(buffer, area);
    std::uint32_t sum = 0;
    const auto has = [](const Cell& cell, CellAttribute attr) {
        return (cell.attributes & static_cast<std::uint32_t>(attr)) != 0;
    };
    for (std::size_t row = 0; row < shape.height; ++row) {
        for (std::size_t col = 0; col < shape.width; ++col) {
            const auto& cell = buffer.at(area.top + row, area.left + col);
            if (cell.continuation || cell.grapheme.empty()) continue;
            const auto cp = ttytk::detail::decode_utf8_scalar(cell.grapheme, 0).value;
            if (cp == U' ' && cell.attributes == 0) continue;
            sum += cp & 0xFFU;
            if (has(cell, CellAttribute::Conceal)) sum += 0x08U;
            if (has(cell, CellAttribute::Underline)) sum += 0x10U;
            if (has(cell, CellAttribute::Inverse)) sum += 0x20U;
            if (has(cell, CellAttribute::Blink) || has(cell, CellAttribute::RapidBlink)) sum += 0x40U;
            if (has(cell, CellAttribute::Bold)) sum += 0x80U;
        }
    }
    return static_cast<std::uint16_t>(0U - sum);
}

/** Checksum using an indexed four-coordinate object. */
template <typename RegionLike>
[[nodiscard]] inline auto checksum(const CellBuffer& buffer, const RegionLike& area)
    noexcept -> std::uint16_t {
    return checksum(buffer, region(area));
}

}  // namespace ttytk::rect
