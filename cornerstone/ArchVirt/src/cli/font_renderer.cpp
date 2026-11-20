#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "cli/font_renderer.hpp"
#include "boot/data_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace domvirt {

static void blend_pixel(std::vector<std::uint32_t>& frame,
                        unsigned screen_width,
                        unsigned screen_height,
                        int x, int y,
                        std::uint32_t fg_color,
                        std::uint8_t alpha) {
    if (x < 0 || y < 0 ||
        static_cast<unsigned>(x) >= screen_width ||
        static_cast<unsigned>(y) >= screen_height) {
        return;
    }
    if (alpha == 0) return;
    if (alpha == 255) {
        frame[static_cast<std::size_t>(y) * screen_width +
             static_cast<std::size_t>(x)] = fg_color;
        return;
    }

    const std::size_t idx = static_cast<std::size_t>(y) * screen_width +
                            static_cast<std::size_t>(x);
    const std::uint32_t bg = frame[idx];

    const std::uint32_t r = ((fg_color >> 16) & 0xFF) * alpha +
                            ((bg >> 16) & 0xFF) * (255 - alpha);
    const std::uint32_t g = ((fg_color >> 8) & 0xFF) * alpha +
                            ((bg >> 8) & 0xFF) * (255 - alpha);
    const std::uint32_t b = (fg_color & 0xFF) * alpha +
                            (bg & 0xFF) * (255 - alpha);

    frame[idx] = 0xFF000000u |
                 ((r / 255) << 16) |
                 ((g / 255) << 8) |
                 (b / 255);
}

FontRenderer::FontRenderer()
    : baked_bitmap_(new unsigned char[BITMAP_SIZE * BITMAP_SIZE]),
      baked_chars_(new stbtt_bakedchar[NUM_CHARS]) {
    std::memset(baked_bitmap_, 0, BITMAP_SIZE * BITMAP_SIZE);
}

FontRenderer::~FontRenderer() {
    delete[] static_cast<unsigned char*>(baked_bitmap_);
    delete[] static_cast<stbtt_bakedchar*>(baked_chars_);
    delete[] static_cast<unsigned char*>(font_data_);
}

bool FontRenderer::load(const char* ttf_path, float font_size) {
    loaded_ = false;
    font_size_ = font_size;
    baked_font_size_ = font_size * BAKE_SCALE;
    delete[] static_cast<unsigned char*>(font_data_);
    font_data_ = nullptr;
    std::memset(baked_bitmap_, 0, BITMAP_SIZE * BITMAP_SIZE);
    std::memset(baked_chars_, 0, sizeof(stbtt_bakedchar) * NUM_CHARS);

    FILE* f = std::fopen(ttf_path, "rb");
    if (!f) {
        std::fprintf(stderr, "FontRenderer: cannot open %s\n", ttf_path);
        return false;
    }

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    auto* ttf_buffer = new unsigned char[size];
    if (std::fread(ttf_buffer, 1, static_cast<std::size_t>(size), f) !=
        static_cast<std::size_t>(size)) {
        std::fclose(f);
        delete[] ttf_buffer;
        std::fprintf(stderr, "FontRenderer: failed to read %s\n", ttf_path);
        return false;
    }
    std::fclose(f);

    font_data_ = ttf_buffer;

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_buffer, 0)) {
        std::fprintf(stderr, "FontRenderer: stbtt_InitFont failed\n");
        return false;
    }

    const float scale = stbtt_ScaleForPixelHeight(&font, font_size);
    int ascent_i = 0, descent_i = 0, line_gap_i = 0;
    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &line_gap_i);
    ascent_ = std::round(static_cast<float>(ascent_i) * scale);
    descent_ = std::round(static_cast<float>(descent_i) * scale);
    line_gap_ = std::round(static_cast<float>(line_gap_i) * scale);

    const int result = stbtt_BakeFontBitmap(
        ttf_buffer, 0,
        baked_font_size_,
        static_cast<unsigned char*>(baked_bitmap_),
        BITMAP_SIZE, BITMAP_SIZE,
        FIRST_CHAR, NUM_CHARS,
        static_cast<stbtt_bakedchar*>(baked_chars_));

    if (result <= 0) {
        std::fprintf(stderr,
                     "FontRenderer: stbtt_BakeFontBitmap failed at "
                     "font size %.1f (returned %d)\n",
                     font_size, result);
        return false;
    }

    loaded_ = true;
    return true;
}

void FontRenderer::draw_text(std::vector<std::uint32_t>& frame,
                             unsigned screen_width,
                             unsigned screen_height,
                             float x,
                             float y,
                             const std::string& text,
                             std::uint32_t color,
                             float scale) const {
    if (!loaded() || text.empty()) return;

    const auto* chars = static_cast<const stbtt_bakedchar*>(baked_chars_);
    const auto* bitmap = static_cast<const unsigned char*>(baked_bitmap_);

    float pen_x = x;
    const float glyph_scale = std::max(0.01f, scale) / BAKE_SCALE;
    const float baseline = y + ascent_ * std::max(0.01f, scale);

    const auto sample_alpha = [bitmap](float x, float y,
                                       const stbtt_bakedchar& glyph) {
        // stbtt_BakeFontBitmap leaves a transparent pixel around each glyph.
        // Include that padding while filtering so fractional edge pixels blend
        // against the background instead of replicating the edge alpha.
        const float x_min = static_cast<float>(std::max(0, glyph.x0 - 1));
        const float x_max = static_cast<float>(
            std::min(BITMAP_SIZE - 1, static_cast<int>(glyph.x1)));
        const float y_min = static_cast<float>(std::max(0, glyph.y0 - 1));
        const float y_max = static_cast<float>(
            std::min(BITMAP_SIZE - 1, static_cast<int>(glyph.y1)));
        const float sx = std::clamp(x, x_min, x_max);
        const float sy = std::clamp(y, y_min, y_max);
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)),
                                  static_cast<int>(x_min),
                                  static_cast<int>(x_max));
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)),
                                  static_cast<int>(y_min),
                                  static_cast<int>(y_max));
        const int x1 = std::min(x0 + 1, static_cast<int>(x_max));
        const int y1 = std::min(y0 + 1, static_cast<int>(y_max));
        const float tx = sx - static_cast<float>(x0);
        const float ty = sy - static_cast<float>(y0);
        const auto at = [bitmap](int px, int py) {
            return static_cast<float>(bitmap[py * BITMAP_SIZE + px]);
        };
        const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
        const float bottom = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
        return static_cast<std::uint8_t>(std::clamp(
            std::lround(top * (1.0f - ty) + bottom * ty), 0l, 255l));
    };

    for (char c : text) {
        const int ci = static_cast<unsigned char>(c) - FIRST_CHAR;
        if (ci < 0 || ci >= NUM_CHARS) {
            continue;
        }

        const stbtt_bakedchar& glyph = chars[ci];
        const float source_width = static_cast<float>(glyph.x1 - glyph.x0);
        const float source_height = static_cast<float>(glyph.y1 - glyph.y0);
        const float x0 = pen_x + glyph.xoff * glyph_scale;
        const float y0 = baseline + glyph.yoff * glyph_scale;
        const float x1 = x0 + source_width * glyph_scale;
        const float y1 = y0 + source_height * glyph_scale;

        if (x1 <= x0 || y1 <= y0 || source_width <= 0.0f || source_height <= 0.0f) {
            pen_x += glyph.xadvance * glyph_scale;
            continue;
        }

        const int px0 = static_cast<int>(std::floor(x0));
        const int py0 = static_cast<int>(std::floor(y0));
        const int px1 = static_cast<int>(std::ceil(x1));
        const int py1 = static_cast<int>(std::ceil(y1));
        const float dst_width = x1 - x0;
        const float dst_height = y1 - y0;

        for (int py = py0; py < py1; ++py) {
            const float v = (static_cast<float>(py) + 0.5f - y0) / dst_height;
            const float source_y = static_cast<float>(glyph.y0) +
                                   v * source_height - 0.5f;
            for (int px = px0; px < px1; ++px) {
                const float u = (static_cast<float>(px) + 0.5f - x0) / dst_width;
                const float source_x = static_cast<float>(glyph.x0) +
                                       u * source_width - 0.5f;
                const std::uint8_t alpha = sample_alpha(source_x, source_y, glyph);
                blend_pixel(frame, screen_width, screen_height, px, py, color, alpha);
            }
        }

        pen_x += glyph.xadvance * glyph_scale;
    }
}

FontRenderer& default_font_renderer() {
    static FontRenderer instance;
#ifdef DOMVIRT_FONT_PATH
    static const bool initialized = instance.load(data_path("fonts/Cousine-Regular.ttf", DOMVIRT_FONT_PATH).c_str(), 15.0f);
    (void)initialized;
#endif
    return instance;
}

} // namespace domvirt
