#include <TTyTk/TTyTk-OSCTools.hpp>

#include <cassert>

int main() {
    assert(ttytk::osc::set_title("TTy") == "\x1B]2;TTy\x1B\\");
    assert(ttytk::osc::hyperlink("https://example.test", "link") ==
           "\x1B]8;;https://example.test\x1B\\link\x1B]8;;\x1B\\");
    assert(ttytk::osc::desktop_notify("done") == "\x1B]9;done\x1B\\");
    assert(ttytk::osc::palette_query(4) == "\x1B]4;4;?\x1B\\");
    assert(ttytk::osc::palette_set(4, "rgb:12/34/56").value() == "\x1B]4;4;rgb:12/34/56\x1B\\");
    assert(!ttytk::osc::palette_set(4, "?"));
    assert(ttytk::osc::set_title("line\n\xC3\x9C") == "\x1B]2;line\xC3\x9C\x1B\\");
    const auto parsed = ttytk::osc::parse("8;id=link;https://example.test");
    assert(parsed && parsed.value().code == 8 && parsed.value().arguments == "id=link;https://example.test");
    assert(ttytk::osc::parse("104").value().arguments.empty());
    assert(!ttytk::osc::parse("2x;title"));
    assert(!ttytk::osc::parse("9999999999999999999;title"));
}
