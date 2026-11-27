#include <TTyTk/TTyTk-VirtTools.hpp>
#include <cassert>

int main() {
    ttytk::VirtualTerm terminal{3, 4};
    const auto cell = [&](std::size_t row, std::size_t column) -> const ttytk::Cell& {
        return terminal.screen().at(row, column);
    };
    terminal.input("ABCD");
    assert(cell(0, 3).grapheme == "D" && cell(1, 0).grapheme == " ");
    terminal.input("E\nF");
    assert(cell(1, 0).grapheme == "E" && cell(2, 1).grapheme == "F");
    terminal.input("\r\nG");
    assert(cell(0, 0).grapheme == "E" && cell(1, 1).grapheme == "F" && cell(2, 0).grapheme == "G");

    terminal.input("\x1b[");
    auto copy = terminal;
    copy.input("2J");
    assert(copy.screen().at(2, 0).grapheme == " " && cell(2, 0).grapheme == "G");
    terminal.input("2;2H\x1b[1;31mZ\x1b[0m");
    assert(cell(1, 1).grapheme == "Z" && cell(1, 1).attributes == 1);
    assert(cell(1, 1).foreground != ttytk::Cell{}.foreground);
    terminal.input("\x1b[?1049hALT\x1b[?1049l!");
    assert(cell(0, 0).grapheme == "E" && cell(1, 2).grapheme == "!");

    terminal.input("\x1b[H\xE4");
    terminal.input("\xB8\x9B" "e");
    terminal.input("\xCC\x81");
    assert(cell(0, 0).grapheme == "\xE4\xB8\x9B" && cell(0, 1).continuation);
    assert(cell(0, 2).grapheme == "e\xCC\x81");
    assert(terminal.output().ends_with("e\xCC\x81"));

    terminal.input("\x1b[?7l\x1b[1;4HXY");
    assert(cell(0, 3).grapheme == "Y");
    terminal.input("\x1b[2;3r\x1b[?6h\x1b[2;1HQ\x1b" "D");
    assert(cell(0, 3).grapheme == "Y" && cell(1, 0).grapheme == "Q" && cell(2, 0).grapheme == " ");

    terminal.screen().resize(0, 0);
    terminal.input("ignored");
    terminal.screen().resize(1, 1);
    terminal.input("x");
    assert(cell(0, 0).grapheme == "x");

    ttytk::VirtualTerm rendition{1, 4};
    rendition.input("\x1b[5;6;38;2;3;42;99mX\x1b[25;39mY");
    assert((rendition.screen().at(0, 0).foreground == ttytk::color::Rgb{3, 42, 99}));
    assert(rendition.screen().at(0, 0).attributes == (16U | 32U));
    assert(rendition.screen().at(0, 1).attributes == 0);
    rendition.input("\x1b[");
    auto moved = std::move(rendition);
    moved.input("2J");
    assert(moved.screen().at(0, 0).grapheme == " ");
}
