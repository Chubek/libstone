#include <TTyTk/TTyTk-CapTools.hpp>

#include <cassert>

int main() {
    const auto capabilities = ttytk::caps::query("\x1b[?62;1;4c\x1b[>41;1;0c"
        "\x1b[0n\x1b[10;20R\x1bP1$r0m\x1b\\\x1b[?2004;2$y"
        "\x1bP1+r524742=31\x1b\\\x1b_Gi=31;OK\x1b\\");
    assert(capabilities.has_sixel);
    assert(capabilities.primary_device_attributes.size() == 3);
    assert(capabilities.secondary_device_attributes.front() == 41);
    assert(capabilities.device_status == 0);
    assert(capabilities.cursor_position && capabilities.cursor_position->first == 10 &&
           capabilities.cursor_position->second == 20);
    assert(capabilities.sgr_status == "0m");
    assert(capabilities.has_bracketed_paste && capabilities.has_truecolor && capabilities.has_kitty_graphics);
    const auto noise = ttytk::caps::query("Kitty RGB 2004;4;\x1b[?4c\x1b[?2004;0$y\x1bP0+r524742\x1b\\");
    assert(!noise.has_sixel && !noise.has_kitty_graphics && !noise.has_truecolor && !noise.has_bracketed_paste);
    assert(ttytk::caps::request().find("\x1b[?2004$p") != std::string_view::npos);
    bool called = false;
    ttytk::caps::query_async([&called](const ttytk::caps::Info& info) {
        called = info.has_truecolor;
    });
    ttytk::caps::on_response("\x1bP1+r5463\x1b\\");
    assert(called);
    ttytk::caps::on_response("");
    assert(called);
}
