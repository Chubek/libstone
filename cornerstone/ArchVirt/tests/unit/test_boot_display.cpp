#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "cli/boot_display_render.hpp"

using namespace domvirt;

TEST(BootDisplayRenderTest, DetectsVisibleContent) {
    std::vector<std::uint32_t> pixels(128, 0);
    EXPECT_FALSE(framebuffer_has_visible_content(pixels.data(), pixels.size()));
    pixels[17] = 0x00112233u;
    EXPECT_TRUE(framebuffer_has_visible_content(pixels.data(), pixels.size()));
}

TEST(BootDisplayRenderTest, ZeroFilledConsoleIsNotVisible) {
    std::vector<std::uint8_t> text(80u * 25u * 2u, 0);
    EXPECT_FALSE(vga_text_has_visible_content(VgaTextBuffer{text.data(), 80, 25, 160}));
}

TEST(BootDisplayRenderTest, ConsoleTextIsVisible) {
    std::vector<std::uint8_t> text(80u * 25u * 2u, 0);
    text[0] = 'D';
    EXPECT_TRUE(vga_text_has_visible_content(VgaTextBuffer{text.data(), 80, 25, 160}));
}

TEST(BootDisplayRenderTest, ColoredConsoleBackgroundIsVisible) {
    std::vector<std::uint8_t> text(80u * 25u * 2u, 0);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        text[i] = ' ';
        text[i + 1] = 0x1Eu;
    }
    EXPECT_TRUE(vga_text_has_visible_content(VgaTextBuffer{text.data(), 80, 25, 160}));
}

TEST(BootDisplayRenderTest, SpaceOnlyConsoleFillsBackground) {
    std::vector<std::uint32_t> frame(960u * 540u, 0);
    std::vector<std::uint8_t> text(80u * 25u * 2u, 0);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        text[i] = ' ';
        text[i + 1] = 0x1Eu;
    }

    render_vga_text_mode(frame, 960, 540, VgaTextBuffer{ text.data(), 80, 25, 160 });

    const std::uint32_t background = frame.front();
    EXPECT_TRUE(std::all_of(frame.begin(), frame.end(),
                            [background](std::uint32_t pixel) { return pixel == background; }));
}

TEST(BootDisplayRenderTest, GlyphProducesForegroundPixels) {
    std::vector<std::uint32_t> frame(960u * 540u, 0);
    std::vector<std::uint8_t> text(80u * 25u * 2u, 0);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        text[i] = ' ';
        text[i + 1] = 0x1Eu;
    }
    text[0] = 'A';

    render_vga_text_mode(frame, 960, 540, VgaTextBuffer{ text.data(), 80, 25, 160 });

    const std::uint32_t background = frame.front();
    EXPECT_TRUE(std::any_of(frame.begin(), frame.end(),
                            [background](std::uint32_t pixel) { return pixel != background; }));
}
