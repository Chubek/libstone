#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace domvirt {

struct FontRenderer {
    FontRenderer();
    ~FontRenderer();

    FontRenderer(const FontRenderer&) = delete;
    FontRenderer& operator=(const FontRenderer&) = delete;

    bool load(const char* ttf_path, float font_size);
    bool loaded() const { return loaded_; }

    float font_size() const { return font_size_; }
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    float line_height() const { return ascent_ - descent_ + line_gap_; }

    void draw_text(std::vector<std::uint32_t>& frame,
                   unsigned screen_width,
                   unsigned screen_height,
                   float x,
                   float y,
                   const std::string& text,
                   std::uint32_t color,
                   float scale) const;

private:
    static constexpr int BITMAP_SIZE = 512;
    static constexpr int FIRST_CHAR = 32;
    static constexpr int NUM_CHARS = 96;
    static constexpr float BAKE_SCALE = 4.0f;

    float font_size_ = 14.0f;
    float baked_font_size_ = 56.0f;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float line_gap_ = 0.0f;
    bool loaded_ = false;

    unsigned char* baked_bitmap_ = nullptr;
    void* baked_chars_ = nullptr;
    void* font_data_ = nullptr;
};

FontRenderer& default_font_renderer();

} // namespace domvirt
