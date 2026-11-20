#include "cli/boot_display_render.hpp"
#include "cli/font_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace domvirt {
namespace {

constexpr std::uint32_t CONSOLE_BG     = 0xFF1E1E1Eu;
constexpr std::uint32_t CONSOLE_TEXT   = 0xFFE0E0E0u;

void fill_rect(std::vector<std::uint32_t>& frame, unsigned width, unsigned height,
               unsigned x, unsigned y, unsigned w, unsigned h, std::uint32_t color) {
    if (x >= width || y >= height || w == 0 || h == 0) {
        return;
    }
    const unsigned x2 = std::min(width, x + w);
    const unsigned y2 = std::min(height, y + h);
    for (unsigned yy = y; yy < y2; ++yy) {
        const auto row = static_cast<std::size_t>(yy) * width;
        for (unsigned xx = x; xx < x2; ++xx) {
            frame[row + xx] = color;
        }
    }
}

} // namespace

bool framebuffer_has_visible_content(const std::uint32_t* pixels, std::size_t pixel_count) {
    if (!pixels || pixel_count == 0) {
        return false;
    }
    return std::any_of(pixels, pixels + pixel_count,
                       [](std::uint32_t pixel) { return (pixel & 0x00FFFFFFu) != 0; });
}

bool vga_text_has_visible_content(const VgaTextBuffer& text) {
    if (text.base == nullptr || text.columns == 0 || text.rows == 0 ||
        text.pitch_bytes < text.columns * 2u) {
        return false;
    }

    for (unsigned row = 0; row < text.rows; ++row) {
        const auto* row_base = text.base + static_cast<std::size_t>(row) * text.pitch_bytes;
        for (unsigned col = 0; col < text.columns; ++col) {
            const std::uint8_t ch = row_base[col * 2u];
            const std::uint8_t attr = row_base[col * 2u + 1u];
            if ((ch != 0 && ch != static_cast<std::uint8_t>(' ')) ||
                ((attr >> 4u) & 0x0Fu) != 0u) {
                return true;
            }
        }
    }
    return false;
}

void render_vga_text_mode(std::vector<std::uint32_t>& frame,
                          unsigned width,
                          unsigned height,
                          const VgaTextBuffer& text) {
    if (frame.size() < static_cast<std::size_t>(width) * height ||
        text.base == nullptr || text.columns == 0 || text.rows == 0 ||
        text.pitch_bytes < text.columns * 2u) {
        return;
    }

    FontRenderer& font = default_font_renderer();
    if (!font.loaded()) {
        return;
    }

    std::fill(frame.begin(), frame.end(), CONSOLE_BG);

    const float cell_w = static_cast<float>(width) / static_cast<float>(text.columns);
    const float cell_h = static_cast<float>(height) / static_cast<float>(text.rows);
    const float font_h = font.line_height();
    const float vertical_scale = cell_h / std::max(0.01f, font_h);
    const float horizontal_scale =
        cell_w / std::max(0.01f, font.font_size() * 0.6f);
    const float glyph_scale = std::max(
        0.01f, std::min(vertical_scale, horizontal_scale));
    const float glyph_pad_x = std::max(0.0f,
        (cell_w - font.font_size() * 0.6f * glyph_scale) * 0.5f);
    const float glyph_pad_y = std::max(0.0f,
        (cell_h - font_h * glyph_scale) * 0.5f);

    for (unsigned row = 0; row < text.rows; ++row) {
        const unsigned y0 = static_cast<unsigned>(std::lround(row * cell_h));
        const unsigned y1 = static_cast<unsigned>(std::lround((row + 1u) * cell_h));
        const unsigned h = std::max(1u, y1 - y0);
        const auto* row_base = text.base +
            static_cast<std::size_t>(row) * text.pitch_bytes;

        for (unsigned col = 0; col < text.columns; ++col) {
            const unsigned x0 = static_cast<unsigned>(std::lround(col * cell_w));
            const unsigned x1 = static_cast<unsigned>(std::lround((col + 1u) * cell_w));
            const unsigned w = std::max(1u, x1 - x0);
            const std::uint8_t ch = row_base[col * 2u];
            fill_rect(frame, width, height, x0, y0, w, h, CONSOLE_BG);

            if (ch >= 32 && ch <= 126) {
                const std::string cell_text(1, static_cast<char>(ch));
                font.draw_text(frame, width, height,
                               static_cast<float>(x0) + glyph_pad_x,
                               static_cast<float>(y0) + glyph_pad_y,
                               cell_text, CONSOLE_TEXT, glyph_scale);
            }
        }
    }
}

} // namespace domvirt
