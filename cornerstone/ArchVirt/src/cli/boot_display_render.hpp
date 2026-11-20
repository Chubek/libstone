#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace domvirt {

struct VgaTextBuffer {
    const std::uint8_t* base{nullptr};
    unsigned columns{80};
    unsigned rows{25};
    unsigned pitch_bytes{160};
};

bool framebuffer_has_visible_content(const std::uint32_t* pixels, std::size_t pixel_count);

bool vga_text_has_visible_content(const VgaTextBuffer& text);

void render_vga_text_mode(std::vector<std::uint32_t>& frame,
                          unsigned width,
                          unsigned height,
                          const VgaTextBuffer& text);

} // namespace domvirt
