#include <TTyTk/TTyTk-RectOps.hpp>

#include <array>
#include <cassert>
#include <limits>

int main() {
    using namespace ttytk;
    CellBuffer buffer{2, 6};
    constexpr auto edge = std::numeric_limits<std::size_t>::max();
    rect::fill(buffer, {0, 0, 1, edge}, U'!', 0);
    rect::fill(buffer, std::array{0, 0, 0, 2}, U'\u00E9', 8);
    assert(buffer.at(0, 2).grapheme == "\xC3\xA9");
    assert(buffer.at(0, 2).attributes == 8);
    assert(rect::checksum(buffer, {0, 0, 0, 0}) == static_cast<std::uint16_t>(-(0xe9 + 0x10)));
    rect::copy(buffer, {0, 0, 0, 3}, 0, 1);
    assert(buffer.at(0, 3).grapheme == "\xC3\xA9");
    assert(buffer.at(0, 4).grapheme == "!");
    rect::copy(buffer, {0, 3, edge, edge}, {1, 5, edge, edge});
    assert(buffer.at(1, 5).grapheme == "\xC3\xA9");

    rect::fill(buffer, {1, 0, 1, 2}, U'\u5B57', 1);
    assert(buffer.at(1, 0).grapheme == "\xE5\xAD\x97");
    assert(buffer.at(1, 1).continuation);
    assert(buffer.at(1, 2).grapheme == " ");
    rect::copy(buffer, {1, 0, 1, 1}, {0, 4, 0, 5});
    assert(buffer.at(0, 5).continuation);
    assert(buffer.at(0, 4).attributes == 1);
    rect::copy(buffer, {0, 4, 0, 5}, 1, 5);
    assert(buffer.at(1, 5) == Cell{});
    rect::erase(buffer, {0, 5, 0, 5});
    assert(buffer.at(0, 4) == Cell{});
    assert(buffer.at(0, 5) == Cell{});
    rect::erase(buffer, {edge, 0, edge, 0});
    assert(rect::checksum(buffer, {1, 3, 0, 5}) == 0);
    assert(rect::checksum(buffer, {0, 4, 0, 5}) == 0);
    const color::Rgb red{255, 0, 0};
    buffer.put(1, 4, Cell{"A", 7, red});
    rect::fill(buffer, {1, 4, 1, 4}, U'X', 3);
    assert(buffer.at(1, 4).foreground == red && buffer.at(1, 4).attributes == 3);
    rect::erase(buffer, {1, 4, 1, 4});
    assert(buffer.at(1, 4).foreground == red && buffer.at(1, 4).attributes == 0);
}
