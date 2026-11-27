#include <TTyTk/TTyTk-NotifyTools.hpp>
#include <cassert>
#include <limits>

int main() {
    using namespace ttytk::notify;
    assert(show("title", "body") == "\x1b]777;notify;title;body\x1b\\");
    assert(show("a;\x07", "body;\x1b\\\x9c丛") == "\x1b]777;notify;a ;body;\\丛\x1b\\");
    assert(progress(1, 2) == "\x1b]9;4;1;50\x1b\\");
    assert(progress(9, 0) == "\x1b]9;4;1;0\x1b\\");
    const auto max = std::numeric_limits<unsigned>::max();
    assert(progress(max - 1, max) == "\x1b]9;4;1;99\x1b\\");
    assert(progress(max, 1) == "\x1b]9;4;1;100\x1b\\");
    assert(badge("A") == "\x1b]1337;SetBadgeFormat=QQ==\x07");
    assert(badge("AB") == "\x1b]1337;SetBadgeFormat=QUI=\x07");
    assert(badge("AB\x07") == "\x1b]1337;SetBadgeFormat=QUIH\x07");
}
