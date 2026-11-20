#include "cli/boot_display.hpp"
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>

int main() {
    domvirt::BootDisplay display;
    for (unsigned i = 0; i < 200 && !display.available(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!display.available()) { std::cerr << "ImGui/SDL display did not initialize\n"; return 1; }
    display.set_status("ImGui smoke test", 0.5f);
    {
        std::vector<std::uint8_t> text(4000);
        for (unsigned i = 0; i < text.size(); i += 2) { text[i] = 'A'; text[i + 1] = 0x1f; }
        display.set_guest_text_buffer(text.data(), 80, 25, 160);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
        std::vector<std::uint32_t> pixels(64 * 64, 0x00aaff);
        display.set_guest_framebuffer(pixels.data(), 64, 64, 256, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    display.set_guest_framebuffer(nullptr, 0, 0, 0, 0);
    display.set_guest_text_buffer(nullptr, 0, 0, 0);
    display.stop();
    return display.available() ? 1 : 0;
}
