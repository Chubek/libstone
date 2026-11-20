#include "presentation_plane.hpp"

#include <algorithm>

namespace domios {

namespace {

constexpr std::uint8_t D_GLYPH[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
constexpr std::uint8_t O_GLYPH[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
constexpr std::uint8_t M_GLYPH[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
constexpr std::uint8_t I_GLYPH[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
constexpr std::uint8_t S_GLYPH[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};

constexpr std::uint8_t glyph_row(char glyph, unsigned row) {
    // Five-pixel-wide, seven-row glyphs used for the firmware wordmark.
    switch (glyph) {
    case 'D': return D_GLYPH[row];
    case 'O': return O_GLYPH[row];
    case 'M': return M_GLYPH[row];
    case 'I': return I_GLYPH[row];
    case 'S': return S_GLYPH[row];
    default: return 0;
    }
}

void draw_wordmark(DrawingSurface& surface, Geometry geometry, const char* text,
                   std::uint32_t color, std::uint32_t scale) {
    if (!text || scale == 0) {
        return;
    }
    constexpr std::uint32_t glyph_width = 5;
    constexpr std::uint32_t glyph_height = 7;
    std::uint32_t length = 0;
    for (const char* p = text; *p; ++p) {
        length += glyph_width;
        if (p[1]) {
            ++length;
        }
    }
    const std::uint32_t word_width = length * scale;
    const std::uint32_t word_height = glyph_height * scale;
    const std::uint32_t x0 = geometry.width > word_width ? (geometry.width - word_width) / 2u : 0u;
    const std::uint32_t y0 = geometry.height > word_height ? (geometry.height - word_height) / 2u : 0u;

    std::uint32_t cursor = x0;
    for (const char* p = text; *p; ++p) {
        for (std::uint32_t row = 0; row < glyph_height; ++row) {
            const auto bits = glyph_row(*p, row);
            for (std::uint32_t col = 0; col < glyph_width; ++col) {
                if ((bits & (1u << (glyph_width - 1u - col))) != 0u) {
                    surface.fill_rect(cursor + col * scale, y0 + row * scale,
                                      scale, scale, color);
                }
            }
        }
        cursor += glyph_width * scale + scale;
    }
}

} // namespace

PresentationPlane::PresentationPlane(GraphicsBackend& backend) : backend_(backend) {}

bool PresentationPlane::render(const ControlPlane&, EventQueue&, std::string& error) {
    const auto geometry = backend_.geometry();
    if (geometry.width == 0 || geometry.height == 0) {
        error = "invalid graphics geometry";
        return false;
    }
    auto surface = backend_.acquire_surface();
    if (!surface) {
        error = "no surface";
        return false;
    }
    constexpr std::uint32_t background = 0x0010182Au;
    constexpr std::uint32_t panel = 0x001A2945u;
    constexpr std::uint32_t accent = 0x0058A0FFu;
    constexpr std::uint32_t accent_soft = 0x003C6096u;
    surface->clear(background);
    surface->fill_rect(0, 0, geometry.width, 8, accent_soft);
    surface->fill_rect(0, geometry.height - 8, geometry.width, 8, accent_soft);

    const std::uint32_t panel_width = geometry.width * 3u / 4u;
    const std::uint32_t panel_height = geometry.height / 2u;
    const std::uint32_t panel_x = (geometry.width - panel_width) / 2u;
    const std::uint32_t panel_y = (geometry.height - panel_height) / 2u;
    surface->fill_rect(panel_x, panel_y, panel_width, panel_height, panel);
    surface->fill_rect(panel_x, panel_y, panel_width, 4, accent_soft);
    surface->fill_rect(panel_x, panel_y + panel_height - 4, panel_width, 4, accent_soft);

    const std::uint32_t scale = std::max(2u, std::min(geometry.width / 70u, geometry.height / 35u));
    draw_wordmark(*surface, geometry, "DOMIOS", 0x00EAF3FFu, scale);

    const std::uint32_t bar_width = panel_width * 2u / 3u;
    const std::uint32_t bar_height = std::max(4u, scale * 2u);
    const std::uint32_t bar_x = (geometry.width - bar_width) / 2u;
    const std::uint32_t bar_y = panel_y + panel_height * 3u / 4u;
    surface->fill_rect(bar_x, bar_y, bar_width, bar_height, 0x00243858u);
    surface->fill_rect(bar_x, bar_y, bar_width / 2u, bar_height, accent);
    if (!backend_.present(*surface)) {
        error = "present failed";
        return false;
    }
    return true;
}

} // namespace domios
