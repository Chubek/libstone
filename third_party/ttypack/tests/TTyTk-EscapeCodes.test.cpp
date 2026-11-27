#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <cassert>

int main() {
    assert(ttytk::esc::csi == "\x1B[");
    assert(ttytk::esc::st == "\x1B\\");
    assert(ttytk::esc::sgr(ttytk::esc::Bold) == "\x1B[1m");
    assert(ttytk::esc::sgr({1, 38, 5, 67}) == "\x1B[1;38;5;67m");
    assert(ttytk::esc::cursor_to(5, 10) == "\x1B[5;10H");
    assert(ttytk::esc::cursor_to(0, 0) == "\x1B[1;1H");
    assert(ttytk::esc::clear() == "\x1B[2J");
    assert(ttytk::Hyperlink::open("https://example.test") == "\x1B]8;;https://example.test\x1B\\");
    assert(ttytk::Hyperlink::open("line\nbreak") == ttytk::Hyperlink::close());
}
